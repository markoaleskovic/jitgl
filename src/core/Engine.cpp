#include "core/Engine.h"
#include "ui/EditorUI.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <streambuf>

class ConsoleRedirector : public std::streambuf {
public:
    ConsoleRedirector(EditorUI* uiInstance) : ui(uiInstance) {}

protected:
    virtual std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::string str(s, n);
        if (str != "\n") { 
            ui->AddConsoleOutput(str);
        }
        return n;
    }

    virtual int overflow(int c) override {
        if (c != EOF) {
            char ch = static_cast<char>(c);
            if (ch != '\n') {
                ui->AddConsoleOutput(std::string(1, ch));
            }
        }
        return c;
    }

private:
    EditorUI* ui;
};

// GLOBAL REDIRECTION STATE
namespace {
    ConsoleRedirector* g_redirector = nullptr;
    std::streambuf* g_oldCoutBuffer = nullptr;
    std::streambuf* g_oldCerrBuffer = nullptr;
}

// ENGINE IMPLEMENTATION
Engine::Engine() : window(nullptr), ui(nullptr) {}

Engine::~Engine() { 
    Shutdown(); 
}

bool Engine::Init() {
    if (!glfwInit()){
        std::cerr << "Failed to initialize GLFW\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(1280, 720, "JITGL", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0){
        std::cerr << "Failed to init OpenGL context\n";
        return false;
    }

    std::cout << "OpenGL " << GLAD_VERSION_MAJOR(version) << "." << GLAD_VERSION_MINOR(version) << " initialized.\n";

    ui = new EditorUI();
    ui->Init(window);
    ui->LoadWorkspace("workspace");

    // Initialize the redirector and save the old buffers globally
    g_redirector = new ConsoleRedirector(ui);
    g_oldCoutBuffer = std::cout.rdbuf(g_redirector);
    g_oldCerrBuffer = std::cerr.rdbuf(g_redirector); // intercept errors too

    std::cout << "Engine Initialized successfully.\n";

    return true;
}

void Engine::Run() {
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
    
        ui->NewFrame();
        ui->Draw();
    
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        
        glViewport(0, 0, display_w, display_h);
        
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ui->Render();

        glfwSwapBuffers(window);
    }
}

void Engine::Shutdown()
{
    // restore the standard terminal buffers BEFORE deleting the UI!
    if (g_oldCoutBuffer) {
        std::cout.rdbuf(g_oldCoutBuffer);
        std::cerr.rdbuf(g_oldCerrBuffer);
        delete g_redirector;
        g_oldCoutBuffer = nullptr;
    }

    if (ui){
        ui->Shutdown();
        delete ui;
        ui = nullptr;
    }

    if (window){
        glfwDestroyWindow(window);
        window = nullptr;
    }

    glfwTerminate();
}