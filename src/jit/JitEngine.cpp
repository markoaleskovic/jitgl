#include "jit/JitEngine.h"
#include "runtime/EngineContext.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ManagedStatic.h"

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

namespace {
    void ShutdownInterpreter(const std::shared_ptr<clang::Interpreter>& interpreter) {
        if (!interpreter) {
            return;
        }

        auto lljitOrErr = interpreter->getExecutionEngine();
        if (!lljitOrErr) {
            llvm::consumeError(lljitOrErr.takeError());
            return;
        }

        clang::IncrementalExecutor& executor = *lljitOrErr;
        if (auto err = executor.cleanUp()) {
            llvm::consumeError(std::move(err));
        }
    }
}

JitProgram::~JitProgram() {
    if (interpreter) {
        ShutdownInterpreter(interpreter);
        interpreter.reset();
    }
}


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
    argStorage_.push_back("-ferror-limit=8");

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
    return true;
}

std::unique_ptr<clang::Interpreter> JitEngine::createInterpreter() const {
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
        if (outputCallback_) outputCallback_("[JIT Error] Failed to create compiler: " + os.str());
        return nullptr;
    }

    auto interpOrErr = clang::Interpreter::create(std::move(*ciOrErr));
    if (!interpOrErr) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(interpOrErr.takeError(), os);
        if (outputCallback_) outputCallback_("[JIT Error] Failed to create interpreter: " + os.str());
        return nullptr;
    }

    return std::move(*interpOrErr);
}

std::shared_ptr<JitProgram> JitEngine::CompileFile(const std::string& filepath) {
    const std::string userCode = readFile(filepath);
    if (userCode.empty()) {
        log("[JIT] Could not read file: " + filepath);
        return nullptr;
    }
    return CompileSource(filepath, userCode);
}

std::shared_ptr<JitProgram> JitEngine::CompileSource(const std::string& sourceName, const std::string& sourceCode) {
    auto stagingInterpreter = createInterpreter();
    if (!stagingInterpreter) {
        return nullptr;
    }

    const std::string fullSource = preamble_ + "\n" + sourceCode;

    auto ptuOrErr = stagingInterpreter->Parse(fullSource);
    if (!ptuOrErr) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(ptuOrErr.takeError(), os);
        log("[JIT Parse Error][" + sourceName + "]\n" + os.str());
        return nullptr;
    }

    if (auto execErr = stagingInterpreter->Execute(*ptuOrErr)) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(std::move(execErr), os);
        log("[JIT Execute Error][" + sourceName + "]\n" + os.str());
        return nullptr;
    }

    JitFunctions functions{};
    if (!lookupFunctions(*stagingInterpreter, &functions)) {
        return nullptr;
    }

    auto keepAliveInterpreter = std::shared_ptr<clang::Interpreter>(stagingInterpreter.release());
    auto program = std::make_shared<JitProgram>();
    program->interpreter = std::move(keepAliveInterpreter);
    program->functions = functions;

    log("[JIT] Hot-swap successful for " + sourceName + ".");
    if (functions.init) log("  -> init()");
    if (functions.update) log("  -> update()");
    if (functions.render) log("  -> renderFrame()");

    return program;
}

bool JitEngine::lookupFunctions(clang::Interpreter& interpreter, JitFunctions* outFunctions) {
    if (!outFunctions) {
        return false;
    }

    // Look up all three entry points. Each is optional.
    // The user only needs to define the ones they care about.

    auto tryLookup = [&](const std::string& name) -> void* {
        auto addrOrErr = interpreter.getSymbolAddress(name);
        if (!addrOrErr) {
            llvm::consumeError(addrOrErr.takeError());
            return nullptr;
        }
        return reinterpret_cast<void*>(addrOrErr->getValue());
    };

    outFunctions->init = reinterpret_cast<JitInitFn>(tryLookup("init"));
    outFunctions->update = reinterpret_cast<JitUpdateFn>(tryLookup("update"));
    outFunctions->render = reinterpret_cast<JitRenderFn>(tryLookup("renderFrame"));

    if (!outFunctions->init && !outFunctions->update && !outFunctions->render) {
        log("[JIT] Warning: compiled successfully but found no entry points "
            "(init / update / renderFrame). Did you forget extern \"C\"?");
        return false;
    }

    return true;
}

void JitEngine::SetOutputCallback(std::function<void(const std::string&)> cb) {
    outputCallback_ = std::move(cb);
}

void JitEngine::Terminate() {
    if (terminated_) {
        return;
    }
    terminated_ = true;

    log("[JIT] Terminating JIT engine.");

    outputCallback_ = nullptr;
    preamble_.clear();
    argStorage_.clear();

    llvm::llvm_shutdown();
}
