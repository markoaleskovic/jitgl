// FileWatcher.h
#pragma once
#include <string>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>

class FileWatcher {
public:
    using Callback = std::function<void(const std::string& filepath)>;

    FileWatcher(std::string watchDir, Callback cb);
    ~FileWatcher();

    void Start();
    void Stop();

private:
    void SeedInitialTimestamps();
    void PollLoop(std::stop_token stopToken);

    std::string watchDir_;
    Callback    callback_;
    std::unordered_map<std::string, std::filesystem::file_time_type> lastWriteTimes_;
    std::jthread watchThread_;
    std::atomic<bool> running_{ false };
    std::mutex callbackMutex_;
};
