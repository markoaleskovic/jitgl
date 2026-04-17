#include "jit/JitEngine.h"
#include "runtime/EngineContext.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h" 

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// --- CMake-injected paths (defined via target_compile_definitions) ---
#ifndef JIT_GLAD_INCLUDE_DIR
#define JIT_GLAD_INCLUDE_DIR ""
#endif
#ifndef JIT_PROJECT_SOURCE_DIR
#define JIT_PROJECT_SOURCE_DIR ""
#endif
#ifndef JIT_CLANG_RESOURCE_DIR
#define JIT_CLANG_RESOURCE_DIR ""
#endif
// ---

JitEngine::JitEngine()  = default;
JitEngine::~JitEngine() = default;

void JitEngine::log(const std::string& msg) {
    if (outputCallback_) outputCallback_(msg);
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool JitEngine::Init(const std::string& preamblePath) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    preamble_ = readFile(preamblePath);
    if (preamble_.empty()) {
        log("[JIT] Warning: preamble file not found or empty: " + preamblePath);
    }

    // clang-repl incremental mode does not honour #pragma once across Parse()
    // calls; strip it so re-runs of the preamble don't cause redefinition errors.
    {
        const std::string pragmaOnce = "#pragma once";
        auto pos = preamble_.find(pragmaOnce);
        while (pos != std::string::npos) {
            preamble_.erase(pos, pragmaOnce.size());
            pos = preamble_.find(pragmaOnce, pos);
        }
    }
    argStorage_.push_back("-std=c++17");
    argStorage_.push_back("-xc++");
    argStorage_.push_back("-O0");
    argStorage_.push_back("-fPIC");
    argStorage_.push_back("-fno-rtti");

    if (JIT_CLANG_RESOURCE_DIR[0] != '\0') {
        argStorage_.push_back("-resource-dir");
        argStorage_.push_back(JIT_CLANG_RESOURCE_DIR);
    }

    argStorage_.push_back("-I/usr/include");
    argStorage_.push_back("-I/usr/local/include");

    std::string gladIncludes = JIT_GLAD_INCLUDE_DIR;
    while (!gladIncludes.empty()) {
        size_t sep = gladIncludes.find('|');
        std::string part = (sep == std::string::npos) ? gladIncludes : gladIncludes.substr(0, sep);
        if (!part.empty()) argStorage_.push_back("-I" + part);
        if (sep == std::string::npos) break;
        gladIncludes.erase(0, sep + 1);
    }

    if (std::string(JIT_PROJECT_SOURCE_DIR).size() > 0) {
        argStorage_.push_back(std::string("-I") + JIT_PROJECT_SOURCE_DIR + "/src");
    }

    log("[JIT] Initialization arguments configured.");
    return recreateInterpreter(); // Do the initial creation
}

bool JitEngine::recreateInterpreter() {

    if (interp_) {
        interp_.reset();
    }
    
    std::vector<const char*> args;
    args.reserve(argStorage_.size());
    for (const auto& s : argStorage_) {
        args.push_back(s.c_str());
    }

    clang::IncrementalCompilerBuilder builder;
    builder.SetCompilerArgs(args);

    auto ciOrErr = builder.CreateCpp();
    if (!ciOrErr) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(ciOrErr.takeError(), os);
        log("[JIT Error] Failed to create compiler: " + os.str());
        return false;
    }

    auto interpOrErr = clang::Interpreter::create(std::move(*ciOrErr));
    if (!interpOrErr) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(interpOrErr.takeError(), os);
        log("[JIT Error] Failed to create interpreter: " + os.str());
        return false;
    }

    // Replace the old interpreter with the fresh one
    interp_ = std::move(*interpOrErr);
    return true;
}

bool JitEngine::ReloadFile(const std::string& filepath) {
    std::string userCode = readFile(filepath);
    if (userCode.empty()) {
        log("[JIT] Could not read file: " + filepath);
        return false;
    }

    // Null the atomics BEFORE destroying the old interpreter.
    // If anything below fails, the engine will safely call no-ops next frame
    // instead of jumping into freed JIT memory.
    currentInit_.store(nullptr,   std::memory_order_release);
    currentUpdate_.store(nullptr, std::memory_order_release);
    currentRender_.store(nullptr, std::memory_order_release);

    if (!recreateInterpreter()) {
        return false;
    }

    std::string fullSource = preamble_ + "\n" + userCode;

    auto ptuOrErr = interp_->Parse(fullSource);
    if (!ptuOrErr) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(ptuOrErr.takeError(), os);
        log("[JIT Parse Error]\n" + os.str());
        return false;
    }

    if (auto execErr = interp_->Execute(*ptuOrErr)) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(std::move(execErr), os);
        log("[JIT Execute Error]\n" + os.str());
        return false;
    }

    return lookupAndSwap();
}

bool JitEngine::lookupAndSwap() {
    // Look up all three entry points. Each is optional.
    // The user only needs to define the ones they care about.
    // Bug prevention: we look up ALL symbols before swapping ANY pointer,
    // so a partial compile doesn't leave the engine in a half-swapped state.

    JitInitFn   newInit   = nullptr;
    JitUpdateFn newUpdate = nullptr;
    JitRenderFn newRender = nullptr;

    auto tryLookup = [&](const std::string& name) -> void* {
        auto addrOrErr = interp_->getSymbolAddress(name);
        if (!addrOrErr) {
            llvm::consumeError(addrOrErr.takeError());
            return nullptr;
        }
        return reinterpret_cast<void*>(addrOrErr->getValue());
    };

    newInit   = reinterpret_cast<JitInitFn>  (tryLookup("init"));
    newUpdate = reinterpret_cast<JitUpdateFn>(tryLookup("update"));
    newRender = reinterpret_cast<JitRenderFn>(tryLookup("renderFrame"));

    if (!newInit && !newUpdate && !newRender) {
        log("[JIT] Warning: compiled successfully but found no entry points "
            "(init / update / renderFrame). Did you forget extern \"C\"?");
        return false;
    }

    // Atomic swap -- safe to call from file watcher thread
    if (newInit)   currentInit_.store(newInit,   std::memory_order_release);
    if (newUpdate) currentUpdate_.store(newUpdate, std::memory_order_release);
    if (newRender) currentRender_.store(newRender, std::memory_order_release);

    log("[JIT] Hot-swap successful.");
    if (newInit)   log("  -> init()");
    if (newUpdate) log("  -> update()");
    if (newRender) log("  -> renderFrame()");

    return true;
}

JitFunctions JitEngine::GetFunctions() const {
    JitFunctions fn;
    fn.init   = currentInit_.load(std::memory_order_acquire);
    fn.update = currentUpdate_.load(std::memory_order_acquire);
    fn.render = currentRender_.load(std::memory_order_acquire);
    return fn;
}

void JitEngine::SetOutputCallback(std::function<void(const std::string&)> cb) {
    outputCallback_ = std::move(cb);
}