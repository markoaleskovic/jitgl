#pragma once
#include <string>
#include <functional>

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    // Fire-and-forget: compiles on a background thread, calls the callback with output.
    void compile(const std::string& source);
    void setOutputCallback(std::function<void(std::string)> cb);
    bool isBusy() const { return busy_; }

private:
    struct Impl;
    Impl* impl_;
    std::function<void(std::string)> outputCallback_;
    bool busy_ = false;
};