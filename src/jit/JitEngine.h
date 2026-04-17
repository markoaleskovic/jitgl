#pragma once
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <vector> // <-- Add this

struct EngineContext;

namespace clang { class Interpreter; }

// JitEngine.h - replace typedefs with:
using JitInitFn   = void(*)(EngineContext*);
using JitUpdateFn = void(*)(EngineContext*);
using JitRenderFn = void(*)(EngineContext*);

struct JitFunctions {
    JitInitFn    init     = nullptr;
    JitUpdateFn  update   = nullptr;
    JitRenderFn  render   = nullptr;
};

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    bool Init(const std::string& preamblePath);
    bool ReloadFile(const std::string& filepath);
    JitFunctions GetFunctions() const;
    void SetOutputCallback(std::function<void(const std::string&)> cb);

private:
    std::unique_ptr<clang::Interpreter> interp_;
    std::function<void(const std::string&)> outputCallback_;
    std::string preamble_;
    
    // --- ADD THESE ---
    std::vector<std::string> argStorage_;
    bool recreateInterpreter();
    // -----------------

    std::atomic<JitInitFn>   currentInit_   { nullptr };
    std::atomic<JitUpdateFn> currentUpdate_ { nullptr };
    std::atomic<JitRenderFn> currentRender_ { nullptr };

    bool lookupAndSwap();
    void log(const std::string& msg);
};