//
// Created by malesko on 17. 04. 2026..
//

#ifndef JIT_IDE_CONSOLEREDIRECTSESSION_H
#define JIT_IDE_CONSOLEREDIRECTSESSION_H

#include <atomic>
#include <string>
#include <thread>

class EditorUI;

class ConsoleRedirectSession {
public:
    ConsoleRedirectSession();
    ~ConsoleRedirectSession();

    ConsoleRedirectSession(const ConsoleRedirectSession&) = delete;
    ConsoleRedirectSession& operator=(const ConsoleRedirectSession&) = delete;

    bool Start(EditorUI* ui);
    void Stop();

private:
    void ReaderLoop();
    void FlushPendingLine();

    bool WaitForPipeData();
    bool ReadAvailableData(char* buffer, std::size_t bufferSize);
    void ConsumeBuffer(const char* buffer, ssize_t count);

    EditorUI* ui_ = nullptr;
    std::thread readerThread_;
    std::atomic<bool> readerRunning_{ false };
    int oldStdoutFd_ = -1;
    int oldStderrFd_ = -1;
    int pipeReadFd_ = -1;
    int pipeWriteFd_ = -1;
    std::string pendingLine_;
    bool active_ = false;
};

#endif //JIT_IDE_CONSOLEREDIRECTSESSION_H
