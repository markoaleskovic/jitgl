// FileWatcher.h
#pragma once
#include <string>
#include <functional>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>

class FileWatcher {
public:
    // detectionTime is captured the instant the poll loop noticed the mtime
    // change. Engine uses it as the starting timestamp for the edit-to-
    // display latency metric.
    using Callback = std::function<void(const std::string& filepath,
                                        std::chrono::steady_clock::time_point detectionTime)>;

    FileWatcher(std::string watchDir, Callback cb);
    ~FileWatcher();

    void Start();
    void Stop();

private:
    // Snapshot current mtimes so Start() does not fire callbacks for existing files.
    void SeedInitialTimestamps();
    // Simple polling watcher used for portability (no inotify/FSEvents dependency).
    void PollLoop(std::stop_token stopToken);

    std::string watchDir_;
    Callback    callback_;
    std::unordered_map<std::string, std::filesystem::file_time_type> lastWriteTimes_;
    std::jthread watchThread_;
    std::atomic<bool> running_{ false };
    std::mutex callbackMutex_;
};
