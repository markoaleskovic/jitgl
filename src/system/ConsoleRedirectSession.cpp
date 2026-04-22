#include "system/ConsoleRedirectSession.h"
#include "ui/EditorUI.h"

#include <cerrno>
#include <cstdio>
#include <iostream>

#include <poll.h>
#include <unistd.h>

namespace {
    constexpr int kStdoutFd = 1;
    constexpr int kStderrFd = 2;
}

ConsoleRedirectSession::ConsoleRedirectSession() = default;

ConsoleRedirectSession::~ConsoleRedirectSession() {
    Stop();
}

bool ConsoleRedirectSession::Start(EditorUI* ui) {
    if (active_ || ui == nullptr) {
        return false;
    }

    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0) {
        return false;
    }

    oldStdoutFd_ = dup(kStdoutFd);
    oldStderrFd_ = dup(kStderrFd);
    if (oldStdoutFd_ < 0 || oldStderrFd_ < 0) {
        if (oldStdoutFd_ >= 0) close(oldStdoutFd_);
        if (oldStderrFd_ >= 0) close(oldStderrFd_);
        close(fds[0]);
        close(fds[1]);
        oldStdoutFd_ = -1;
        oldStderrFd_ = -1;
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();

    if (dup2(fds[1], kStdoutFd) < 0 || dup2(fds[1], kStderrFd) < 0) {
        dup2(oldStdoutFd_, kStdoutFd);
        dup2(oldStderrFd_, kStderrFd);
        close(oldStdoutFd_);
        close(oldStderrFd_);
        close(fds[0]);
        close(fds[1]);
        oldStdoutFd_ = -1;
        oldStderrFd_ = -1;
        return false;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    pipeReadFd_ = fds[0];
    pipeWriteFd_ = fds[1];
    ui_ = ui;

    readerRunning_.store(true);
    readerThread_ = std::thread([this]() { ReaderLoop(); });

    active_ = true;
    return true;
}

void ConsoleRedirectSession::ReaderLoop() {
    char buffer[1024];
    while (readerRunning_.load()) {
        pollfd pfd{};
        pfd.fd = pipeReadFd_;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        const int pollResult = poll(&pfd, 1, 100);
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pollResult == 0) {
            FlushPendingLine();
            continue;
        }
        if ((pfd.revents & POLLHUP) != 0) {
            break;
        }
        if ((pfd.revents & POLLERR) != 0) {
            break;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        const ssize_t n = read(pipeReadFd_, buffer, sizeof(buffer));
        if (n <= 0) {
            if (!readerRunning_.load()) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (ssize_t i = 0; i < n; ++i) {
            const char ch = buffer[i];
            if (ch == '\n') {
                FlushPendingLine();
                continue;
            }
            if (ch != '\r') {
                pendingLine_.push_back(ch);
            }
        }
        FlushPendingLine();
    }

    FlushPendingLine();
}

void ConsoleRedirectSession::FlushPendingLine() {
    if (ui_ && !pendingLine_.empty()) {
        ui_->AddConsoleOutput(pendingLine_);
        pendingLine_.clear();
    }
}

void ConsoleRedirectSession::Stop() {
    if (!active_) {
        return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();

    if (oldStdoutFd_ >= 0) {
        dup2(oldStdoutFd_, kStdoutFd);
    }
    if (oldStderrFd_ >= 0) {
        dup2(oldStderrFd_, kStderrFd);
    }

    if (oldStdoutFd_ >= 0) {
        close(oldStdoutFd_);
        oldStdoutFd_ = -1;
    }
    if (oldStderrFd_ >= 0) {
        close(oldStderrFd_);
        oldStderrFd_ = -1;
    }

    readerRunning_.store(false);

    if (pipeWriteFd_ >= 0) {
        close(pipeWriteFd_);
        pipeWriteFd_ = -1;
    }

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    if (pipeReadFd_ >= 0) {
        close(pipeReadFd_);
        pipeReadFd_ = -1;
    }

    pendingLine_.clear();
    ui_ = nullptr;
    active_ = false;
}
