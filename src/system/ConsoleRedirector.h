//
// Created by malesko on 17. 04. 2026..
//

#ifndef JIT_IDE_CONSOLEREDIRECTOR_H
#define JIT_IDE_CONSOLEREDIRECTOR_H
#include <ios>

class EditorUI;

class ConsoleRedirector : public std::streambuf {
public:
    explicit ConsoleRedirector(EditorUI* uiInstance);

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    int overflow(int c) override;

private:
    EditorUI* ui_;
};


#endif //JIT_IDE_CONSOLEREDIRECTOR_H
