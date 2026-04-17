#include "watcher/FileWatcher.h"
#include <chrono>

namespace fs = std::filesystem;
namespace {
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(250);
}
FileWatcher::FileWatcher(std::string watchDir, Callback cb)
    : watchDir_(std::move(watchDir)), callback_(std::move(cb)) {}

FileWatcher::~FileWatcher() { Stop(); }

void FileWatcher::Start() {
    running_ = true;

    // Seed initial timestamps to avoid false triggers on startup
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(watchDir_, ec)) {
        if (entry.is_regular_file(ec)) {
            lastWriteTimes_[entry.path().string()] = entry.last_write_time(ec);
        }
    }

    watchThread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(POLL_INTERVAL);

            std::error_code ec;
            for (auto& entry : fs::directory_iterator(watchDir_, ec)) {
                if (!entry.is_regular_file(ec)) continue;

                auto path        = entry.path().string();
                auto currentTime = entry.last_write_time(ec);
                if (ec) continue;

                auto it = lastWriteTimes_.find(path);
                if (it != lastWriteTimes_.end() && it->second != currentTime) {
                    it->second = currentTime;
                    callback_(path);
                }
            }
        }
    });
}

void FileWatcher::Stop() {
    running_ = false;
    if (watchThread_.joinable()) watchThread_.join();
}