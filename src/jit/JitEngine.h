#pragma once
#include <string>
#include <functional>
#include <memory>
#include <vector>

struct EngineContext;

namespace clang { class Interpreter; }

using JitInitFn   = void(*)(EngineContext*);
using JitUpdateFn = void(*)(EngineContext*);
using JitRenderFn = void(*)(EngineContext*);

struct JitFunctions {
    JitInitFn    init     = nullptr;
    JitUpdateFn  update   = nullptr;
    JitRenderFn  render   = nullptr;
};

struct JitProgram {
    std::shared_ptr<clang::Interpreter> interpreter;
    JitFunctions functions;
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
    bool lookupFunctions(clang::Interpreter& interpreter, JitFunctions* outFunctions);
    void log(const std::string& msg);
    bool terminated_ = false;
};
