#include "watcher/FileWatcher.h"
#include <chrono>

namespace fs = std::filesystem;
namespace {
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);
}
FileWatcher::FileWatcher(std::string watchDir, Callback cb)
    : watchDir_(std::move(watchDir)), callback_(std::move(cb)) {}

FileWatcher::~FileWatcher() { Stop(); }

void FileWatcher::SeedInitialTimestamps() {
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(
             watchDir_, fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            lastWriteTimes_[entry.path().string()] = entry.last_write_time(ec);
        }
    }
}

void FileWatcher::PollLoop(std::stop_token stopToken) {
    while (running_ && !stopToken.stop_requested()) {
        std::this_thread::sleep_for(POLL_INTERVAL);

        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(
                 watchDir_, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file(ec)) continue;

            auto path        = entry.path().string();
            auto currentTime = entry.last_write_time(ec);
            if (ec) continue;

            auto it = lastWriteTimes_.find(path);
            if (it == lastWriteTimes_.end()) {
                // New files are reported once and then tracked normally.
                lastWriteTimes_[path] = currentTime;
                std::scoped_lock lock(callbackMutex_);
                callback_(path);
                continue;
            }

            if (it->second != currentTime) {
                it->second = currentTime;
                std::scoped_lock lock(callbackMutex_);
                callback_(path);
            }
        }
    }
}

void FileWatcher::Start() {
    running_ = true;
    SeedInitialTimestamps();

    watchThread_ = std::jthread([this](std::stop_token stopToken) {
        PollLoop(stopToken);
    });
}

void FileWatcher::Stop() {
    running_ = false;
    watchThread_.request_stop();
    if (watchThread_.joinable()) {
        watchThread_.join();
    }
}
