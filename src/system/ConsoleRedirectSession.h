//
// Created by malesko on 17. 04. 2026..
//

#ifndef JIT_IDE_CONSOLEREDIRECTSESSION_H
#define JIT_IDE_CONSOLEREDIRECTSESSION_H

#include <memory>
#include <streambuf>

class EditorUI;
class ConsoleRedirector;

class ConsoleRedirectSession {
public:
    ConsoleRedirectSession();
    ~ConsoleRedirectSession();

    ConsoleRedirectSession(const ConsoleRedirectSession&) = delete;
    ConsoleRedirectSession& operator=(const ConsoleRedirectSession&) = delete;

    bool Start(EditorUI* ui);
    void Stop();

private:
    std::unique_ptr<ConsoleRedirector> redirector_;
    std::streambuf* oldCout_ = nullptr;
    std::streambuf* oldCerr_ = nullptr;
    bool active_ = false;
};

#endif //JIT_IDE_CONSOLEREDIRECTSESSION_H
