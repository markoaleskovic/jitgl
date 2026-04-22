#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Include this so std::unique_ptr knows how to delete it
#include "ui/EditorUI.h"
#include "runtime/EngineContext.h"
#include "jit/JitEngine.h"

struct GLFWwindow;
class FileWatcher;
class ConsoleRedirectSession;
class WorkspaceManager;

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

    // Main-thread runtime state
    EngineContext ctx_;
    std::shared_ptr<JitProgram> activeProgram_;
    std::unordered_map<std::string, std::shared_ptr<JitProgram>> compiledPrograms_;
    std::unordered_map<std::string, std::string> latestSources_;
    std::unordered_map<std::string, double> pendingCompilesAt_;
    std::unordered_map<std::string, double> ignoreWatcherUntil_;
    std::string activeFilePath_;
    double lastTime_ = 0.0;

    // Scene render target shown in UI renderer tab
    unsigned int sceneFbo_ = 0;
    unsigned int sceneColorTex_ = 0;
    unsigned int sceneDepthRbo_ = 0;
    int sceneWidth_ = 0;
    int sceneHeight_ = 0;

    // Pending file paths posted by watcher thread, consumed on main thread
    std::mutex pendingMutex_;
    std::queue<std::string> pendingReloads_;

    struct CompileJob {
        std::string filepath;
        std::string source;
        std::size_t sourceHash = 0;
    };

    struct CompileResult {
        std::string filepath;
        std::shared_ptr<JitProgram> program;
        std::size_t sourceHash = 0;
    };

    struct CompileFailureState {
        std::size_t sourceHash = 0;
        bool suppressionLogged = false;
    };

    std::mutex compileMutex_;
    std::condition_variable compileCv_;
    std::deque<CompileJob> compileJobs_;
    std::queue<CompileResult> compileResults_;
    std::unordered_map<std::string, std::size_t> inFlightSourceHashes_;
    std::thread compileThread_;
    std::atomic<bool> compileThreadRunning_{ false };
    std::unordered_map<std::string, CompileFailureState> compileFailures_;
    std::unordered_map<std::string, double> compileRetryAfter_;

    void OnFileChanged(const std::string& filepath);
    void ProcessPendingReloads(double nowSeconds);

    void HandleDocumentEdited(const std::string& filepath, const std::string& content);
    void HandleActiveDocumentChanged(const std::string& filepath, const std::string& content);
    void QueueCompile(const std::string& filepath, const std::string& source, double nowSeconds, bool immediate);
    void SubmitDueCompiles(double nowSeconds);
    void ProcessCompileResults();
    void CompileThreadMain();
    bool ActivateProgramForPath(const std::string& filepath);

    bool EnsureSceneRenderTarget(int width, int height);
    void DestroySceneRenderTarget();
    void RenderSceneToTexture();

    bool InitWindow();
    bool InitGL();
    bool InitUI();
    bool InitJIT();
    bool InitWatcher();

    bool shutdown_ = false;
};
