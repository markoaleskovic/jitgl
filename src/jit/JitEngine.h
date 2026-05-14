#pragma once
#include <string>
#include <functional>
#include <memory>
#include <cstdint>
#include <vector>
#include <unordered_map>

struct EngineContext;

namespace clang { class Interpreter; }

using JitInitFn   = void(*)(EngineContext*);
using JitUpdateFn = void(*)(EngineContext*);
using JitRenderFn = void(*)(EngineContext*);
using JitShutdownFn = void(*)(EngineContext*);

struct JitFunctions {
    // Any callback may be null; user code can implement only the hooks it needs.
    JitInitFn    init     = nullptr;
    JitUpdateFn  update   = nullptr;
    JitRenderFn  render   = nullptr;
    JitShutdownFn shutdown = nullptr;
};

struct JitProgram {
    // Interpreter must stay alive as long as function pointers are used.
    std::shared_ptr<clang::Interpreter> interpreter;
    JitFunctions functions;
    bool initialized = false;
    std::uint64_t allocationOwner = 0;
    bool allocationOwnerReleased = false;
    ~JitProgram();
};

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    bool Init(const std::string& preamblePath);
    std::shared_ptr<JitProgram> CompileSource(const std::string& sourceName, const std::string& sourceCode);
    std::shared_ptr<JitProgram> CompileFile(const std::string& filepath);
    void SetOutputCallback(std::function<void(const std::string&)> cb);
    void Terminate();
private:
    std::function<void(const std::string&)> outputCallback_;
    std::string preamble_;

    std::vector<std::string> argStorage_;

    std::unique_ptr<clang::Interpreter> createInterpreter() const;
    bool RunPreflightSyntaxCheck(const std::string& sourceName,
                                 const std::string& fullSource,
                                 std::string* diagnostics) const;
    bool lookupFunctions(const clang::Interpreter& interpreter, JitFunctions* outFunctions);
    void log(const std::string& msg);
    bool terminated_ = false;
};
