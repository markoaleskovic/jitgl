#include "system/ConsoleRedirectSession.h"
#include "system/ConsoleRedirector.h"

#include <iostream>

ConsoleRedirectSession::ConsoleRedirectSession() = default;

ConsoleRedirectSession::~ConsoleRedirectSession() {
    Stop();
}

bool ConsoleRedirectSession::Start(EditorUI* ui) {
    if (active_ || ui == nullptr) {
        return false;
    }

    redirector_ = std::make_unique<ConsoleRedirector>(ui);
    oldCout_ = std::cout.rdbuf(redirector_.get());
    oldCerr_ = std::cerr.rdbuf(redirector_.get());
    active_ = true;
    return true;
}

void ConsoleRedirectSession::Stop() {
    if (!active_) {
        return;
    }

    if (oldCout_) {
        std::cout.rdbuf(oldCout_);
    }
    if (oldCerr_) {
        std::cerr.rdbuf(oldCerr_);
    }

    redirector_.reset();
    oldCout_ = nullptr;
    oldCerr_ = nullptr;
    active_ = false;
}