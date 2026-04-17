#include "system/ConsoleRedirector.h"
#include "ui/EditorUI.h"

ConsoleRedirector::ConsoleRedirector(EditorUI* uiInstance) : ui_(uiInstance) {}

std::streamsize ConsoleRedirector::xsputn(const char* s, std::streamsize n) {
    std::string str(s, static_cast<size_t>(n));
    if (str != "\n") {
        ui_->AddConsoleOutput(str);
    }
    return n;
}

int ConsoleRedirector::overflow(int c) {
    if (c != EOF) {
        char ch = static_cast<char>(c);
        if (ch != '\n') {
            ui_->AddConsoleOutput(std::string(1, ch));
        }
    }
    return c;
}