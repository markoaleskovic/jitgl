#pragma once
#include <mutex>
#include <memory>
#include <queue>
#include <string>

// Include this so std::unique_ptr knows how to delete it
#include "ui/EditorUI.h"

struct GLFWwindow;
class FileWatcher;
class ConsoleRedirectSession;
class WorkspaceManager;
class JitEngine;
class EngineContext;

class Engine {
public:
    Engine();
    ~Engine();

    bool Init();
    void Run();
    void Shutdown();

private:
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<EditorUI> ui_ = nullptr;

    std::unique_ptr<JitEngine>   jit_;
    std::unique_ptr<FileWatcher> watcher_;

    std::unique_ptr<ConsoleRedirectSession> consoleRedirect_;
    std::unique_ptr<WorkspaceManager> workspaceManager_;

    EngineContext _ctx;
    double       lastTime_ = 0.0;

    // Pending reload paths posted by the watcher thread, consumed on the main thread
    std::mutex           pendingMutex_;
    std::queue<std::string> pendingReloads_;

    void OnFileChanged(const std::string& filepath);
    void ProcessPendingReloads();
    void DrawFrame();

    bool InitWindow();
    bool InitGL();
    bool InitUI();
    bool InitJIT();
    bool InitWatcher();
};