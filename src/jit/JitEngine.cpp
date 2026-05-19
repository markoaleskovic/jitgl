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
#include <array>
#include <bit>
#include <filesystem>
#include <format>
#include <chrono>
#include <thread>
#include <system_error>
#include <cstdlib>

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

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
    constexpr int PRECHECK_TIMEOUT_MS = 3000;
    constexpr rlim_t PRECHECK_MEMORY_LIMIT_BYTES = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::size_t DIAGNOSTIC_LIMIT_BYTES = 10000;

    std::string truncateDiagnostics(const std::string& diagnostics) {
        if (diagnostics.size() <= DIAGNOSTIC_LIMIT_BYTES) {
            return diagnostics;
        }
        return std::format("{}\n[Error log truncated...]", diagnostics.substr(0, DIAGNOSTIC_LIMIT_BYTES));
    }

    fs::path buildPreflightTempPath() {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() /
               std::format("jitgl-preflight-{}-{}.cpp", getpid(), timestamp);
    }

    std::string readFdToString(int fd) {
        std::string out;
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
            if (bytesRead <= 0) {
                break;
            }
            out.append(buffer.data(), static_cast<std::size_t>(bytesRead));
        }
        return out;
    }

    void SetPreflightDiagnostics(std::string* diagnostics, const std::string& message) {
        if (diagnostics) {
            *diagnostics = message;
        }
    }

    void RemoveFileIfPresent(const fs::path& path) {
        std::error_code removeError;
        fs::remove(path, removeError);
    }

    struct PreflightWaitResult {
        int status = 0;
        bool timedOut = false;
    };

    PreflightWaitResult WaitForPreflightProcess(pid_t pid, std::chrono::milliseconds timeout) {
        PreflightWaitResult result{};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const pid_t waitResult = waitpid(pid, &result.status, WNOHANG);
            if (waitResult == pid) {
                break;
            }
            if (waitResult < 0) {
                result.status = -1;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timedOut = true;
                kill(pid, SIGKILL);
                waitpid(pid, &result.status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return result;
    }

    [[noreturn]] void RunPreflightChildProcess(const fs::path& tempSourcePath,
                                               const std::vector<std::string>& argStorage,
                                               const std::array<int, 2>& outputPipe) {
        // Child writes diagnostics back through the pipe so parent can surface them in UI logs.
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[0]);
        close(outputPipe[1]);

        struct rlimit memoryLimit {};
        memoryLimit.rlim_cur = PRECHECK_MEMORY_LIMIT_BYTES;
        memoryLimit.rlim_max = PRECHECK_MEMORY_LIMIT_BYTES;
        setrlimit(RLIMIT_AS, &memoryLimit);

        struct rlimit cpuLimit {};
        cpuLimit.rlim_cur = 4;
        cpuLimit.rlim_max = 4;
        setrlimit(RLIMIT_CPU, &cpuLimit);

        std::vector<std::string> args;
        args.reserve(argStorage.size() + 4);
        args.emplace_back("clang++");
        args.emplace_back("-fsyntax-only");
        args.emplace_back("-fno-color-diagnostics");
        for (const auto& arg : argStorage) {
            if (arg != "-xc++") {
                args.emplace_back(arg);
            }
        }
        args.emplace_back(tempSourcePath.string());

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        execvp("clang++", argv.data());
        // Reserved fallback code: parent treats 127 as "clang++ unavailable".
        _exit(127);
    }

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

    template <typename Fn>
    Fn AddressToFunction(std::uintptr_t address) {
        static_assert(sizeof(Fn) == sizeof(std::uintptr_t),
                      "Function pointer size must match address size");
        // Symbol lookup gives an integer address; bit_cast preserves bits without UB-prone casts.
        return std::bit_cast<Fn>(address);
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
        log(std::format("[JIT] Warning: preamble file not found or empty: {}", preamblePath));
    }

    // clang-repl incremental mode does not honour #pragma once across Parse()
    // calls; strip it so re-runs of the preamble don't cause redefinition errors.
    {
        constexpr std::string_view pragmaOnce = "#pragma once";
        std::string rebuilt;
        rebuilt.reserve(preamble_.size());
        std::size_t cursor = 0;
        while (cursor < preamble_.size()) {
            const std::size_t pos = preamble_.find(pragmaOnce, cursor);
            if (pos == std::string::npos) {
                rebuilt.append(preamble_, cursor, std::string::npos);
                break;
            }
            rebuilt.append(preamble_, cursor, pos - cursor);
            cursor = pos + pragmaOnce.size();
        }
        preamble_ = std::move(rebuilt);
    }
    argStorage_.emplace_back("-std=c++20");
    argStorage_.emplace_back("-xc++");
    argStorage_.emplace_back("-O0");
    argStorage_.emplace_back("-fPIC");
    argStorage_.emplace_back("-fno-rtti");
    argStorage_.emplace_back("-ferror-limit=25");
    argStorage_.emplace_back("-fno-spell-checking");
    argStorage_.emplace_back("-fno-caret-diagnostics");
    argStorage_.emplace_back("-fno-show-column");
    argStorage_.emplace_back("-fno-diagnostics-fixit-info");
    argStorage_.emplace_back("-fno-crash-diagnostics");

    if (JIT_CLANG_RESOURCE_DIR[0] != '\0') {
        argStorage_.emplace_back("-resource-dir");
        argStorage_.emplace_back(JIT_CLANG_RESOURCE_DIR);
    }

    argStorage_.emplace_back("-I/usr/include");
    argStorage_.emplace_back("-I/usr/local/include");

    std::string_view gladIncludes = JIT_GLAD_INCLUDE_DIR;
    while (!gladIncludes.empty()) {
        const std::size_t sep = gladIncludes.find('|');
        const std::string_view part = (sep == std::string_view::npos)
            ? gladIncludes
            : gladIncludes.substr(0, sep);
        if (!part.empty()) {
            argStorage_.emplace_back("-I").append(part);
        }
        if (sep == std::string_view::npos) break;
        gladIncludes.remove_prefix(sep + 1);
    }

    if (!std::string(JIT_PROJECT_SOURCE_DIR).empty()) {
        argStorage_.emplace_back(std::string("-I") + JIT_PROJECT_SOURCE_DIR + "/src");
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

bool JitEngine::RunPreflightSyntaxCheck(const std::string& sourceName,
                                        const std::string& fullSource,
                                        std::string* diagnostics) const {
    if (diagnostics) {
        diagnostics->clear();
    }

    const fs::path tempSourcePath = buildPreflightTempPath();
    {
        std::ofstream out(tempSourcePath, std::ios::binary);
        if (!out.is_open()) {
            SetPreflightDiagnostics(diagnostics,
                                    std::format("[JIT Preflight Error][{}] Failed to create temporary source file.",
                                                sourceName));
            return false;
        }
        out << fullSource;
    }

    std::array<int, 2> outputPipe{ -1, -1 };
    if (pipe(outputPipe.data()) != 0) {
        RemoveFileIfPresent(tempSourcePath);
        SetPreflightDiagnostics(diagnostics,
                                std::format("[JIT Preflight Error][{}] Failed to create output pipe.", sourceName));
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        RemoveFileIfPresent(tempSourcePath);
        SetPreflightDiagnostics(diagnostics,
                                std::format("[JIT Preflight Error][{}] Failed to fork preflight process.", sourceName));
        return false;
    }

    if (pid == 0) {
        RunPreflightChildProcess(tempSourcePath, argStorage_, outputPipe);
    }

    close(outputPipe[1]);

    // Preflight runs out-of-process so syntax failures/crashes never poison the live interpreter.
    const PreflightWaitResult waitResult = WaitForPreflightProcess(pid, std::chrono::milliseconds(PRECHECK_TIMEOUT_MS));
    std::string preflightOutput = readFdToString(outputPipe[0]);
    close(outputPipe[0]);

    RemoveFileIfPresent(tempSourcePath);

    if (waitResult.timedOut) {
        SetPreflightDiagnostics(diagnostics,
                                std::format("[JIT Preflight Error][{}] Syntax preflight timed out after {}ms.\n{}",
                                            sourceName,
                                            PRECHECK_TIMEOUT_MS,
                                            truncateDiagnostics(preflightOutput)));
        return false;
    }

    if (waitResult.status == -1) {
        SetPreflightDiagnostics(diagnostics,
                                std::format("[JIT Preflight Error][{}] waitpid failed.", sourceName));
        return false;
    }

    if (!WIFEXITED(waitResult.status)) {
        SetPreflightDiagnostics(
            diagnostics,
            std::format("[JIT Preflight Error][{}] Preflight process did not exit cleanly.\n{}",
                        sourceName,
                        truncateDiagnostics(preflightOutput)));
        return false;
    }

    const int exitCode = WEXITSTATUS(waitResult.status);
    if (exitCode == 127) {
        // clang++ is unavailable; keep old behavior instead of blocking all compiles.
        return true;
    }

    if (exitCode != 0) {
        SetPreflightDiagnostics(diagnostics,
                                std::format("[JIT Preflight Error][{}]\n{}",
                                            sourceName,
                                            truncateDiagnostics(preflightOutput)));
        return false;
    }

    return true;
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
    log(std::format("[JIT] Starting compilation for {}...", sourceName));
    const std::string fullSource = preamble_ + "\n" + sourceCode;

    std::string preflightDiagnostics;
    if (!RunPreflightSyntaxCheck(sourceName, fullSource, &preflightDiagnostics)) {
        log(preflightDiagnostics);
        return nullptr;
    }

    auto stagingInterpreter = createInterpreter();
    if (!stagingInterpreter) {
        return nullptr;
    }

    log("[JIT] Parsing source...");
    auto ptuOrErr = stagingInterpreter->Parse(fullSource);
    if (!ptuOrErr) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(ptuOrErr.takeError(), os);
        
        msg = truncateDiagnostics(msg);
        
        log(std::format("[JIT Parse Error][{}]\n{}", sourceName, msg));
        return nullptr;
    }

    log("[JIT] Executing PTU...");
    if (auto execErr = stagingInterpreter->Execute(*ptuOrErr)) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(std::move(execErr), os);
        log(std::format("[JIT Execute Error][{}]\n{}", sourceName, os.str()));
        return nullptr;
    }

    JitFunctions functions{};
    if (!lookupFunctions(*stagingInterpreter, &functions)) {
        return nullptr;
    }

    auto keepAliveInterpreter = std::shared_ptr<clang::Interpreter>(stagingInterpreter.release());
    auto program = std::make_shared<JitProgram>();
    // Program keeps interpreter alive so resolved function pointers remain valid.
    program->interpreter = std::move(keepAliveInterpreter);
    program->functions = functions;

    log(std::format("[JIT] Hot-swap successful for {}.", sourceName));
    if (functions.init) log("  -> init()");
    if (functions.update) log("  -> update()");
    if (functions.compute) log("  -> dispatchCompute()");
    if (functions.render) log("  -> renderFrame()");
    if (functions.shutdown) log("  -> shutdown()");

    return program;
}

bool JitEngine::lookupFunctions(const clang::Interpreter& interpreter, JitFunctions* outFunctions) {
    if (!outFunctions) {
        return false;
    }

    // Look up all three entry points. Each is optional.
    // The user only needs to define the ones they care about.

    auto tryLookup = [&](const std::string& name) -> std::uintptr_t {
        auto addrOrErr = interpreter.getSymbolAddress(name);
        if (!addrOrErr) {
            llvm::consumeError(addrOrErr.takeError());
            return 0;
        }
        return static_cast<std::uintptr_t>(addrOrErr->getValue());
    };

    outFunctions->init = AddressToFunction<JitInitFn>(tryLookup("init"));
    outFunctions->update = AddressToFunction<JitUpdateFn>(tryLookup("update"));
    outFunctions->compute = AddressToFunction<JitComputeFn>(tryLookup("dispatchCompute"));
    outFunctions->render = AddressToFunction<JitRenderFn>(tryLookup("renderFrame"));
    outFunctions->shutdown = AddressToFunction<JitShutdownFn>(tryLookup("shutdown"));

    if (!outFunctions->init &&
        !outFunctions->update &&
        !outFunctions->compute &&
        !outFunctions->render &&
        !outFunctions->shutdown) {
        log("[JIT] Warning: compiled successfully but found no entry points "
            "(init / update / dispatchCompute / renderFrame / shutdown). Did you forget extern \"C\"?");
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

    // llvm_shutdown() will clean up LLVM's global state.
    // Ensure all shared_ptr<clang::Interpreter> are already destroyed before this call!
    llvm::llvm_shutdown();
}
