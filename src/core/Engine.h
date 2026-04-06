#pragma once


struct GLFWwindow;

class EditorUI;
//class JitEngine
//class FileWatcher
//class Renderer

class Engine {
public:
    Engine();
    ~Engine();


    bool Init();
    void Run();
    void Shutdown();

private:
    GLFWwindow* window;
    EditorUI* ui;

    //JitEngine* jitEngine;
    //FileWatcher* fileWatcher;
    //Renderer* renderer;
    
};
