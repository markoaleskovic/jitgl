#include "system/ConsoleRedirector.h"
#include "ui/EditorUI.h"

ConsoleRedirector::ConsoleRedirector(EditorUI* uiInstance) : ui_(uiInstance) {}

std::streamsize ConsoleRedirector::xsputn(const char* s, std::streamsize n) {
    std::scoped_lock<std::mutex> lock(bufferMutex_);
    for (std::streamsize i = 0; i < n; ++i) {
        const char ch = s[i];
        if (ch == '\n') {
            if (!pendingLine_.empty()) {
                ui_->AddConsoleOutput(pendingLine_);
                pendingLine_.clear();
            }
        } else {
            pendingLine_.push_back(ch);
        }
    }
    return n;
}

int ConsoleRedirector::overflow(int c) {
    if (c == EOF) {
        return c;
    }

    const auto ch = static_cast<char>(c);
    std::scoped_lock<std::mutex> lock(bufferMutex_);
    if (ch == '\n') {
        if (!pendingLine_.empty()) {
            ui_->AddConsoleOutput(pendingLine_);
            pendingLine_.clear();
        }
    } else {
        pendingLine_.push_back(ch);
    }

    return c;
}
