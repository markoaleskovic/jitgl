#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <array>
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
#include "system/network/LanWorkspaceShareService.h"

struct GLFWwindow;
class FileWatcher;
class ConsoleRedirectSession;
class WorkspaceManager;
struct WorkspaceDescriptor;

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

    std::shared_ptr<JitEngine>   jit_;
    std::unique_ptr<FileWatcher> watcher_;

    std::unique_ptr<ConsoleRedirectSession> consoleRedirect_;
    std::unique_ptr<WorkspaceManager> workspaceManager_;
    std::unique_ptr<LanWorkspaceShareService> lanShare_;

    // Main-thread runtime state
    EngineContext ctx_;
    std::shared_ptr<JitProgram> activeProgram_;
    std::unordered_map<std::string, std::shared_ptr<JitProgram>> compiledPrograms_;
    struct WorkspaceState {
        std::string name;
        std::string directory;
        std::string cppPath;
        std::string shaderPath;
        std::string consoleLogPath;
        std::string engineLogPath;
        std::string cppSource;
        std::string shaderSource;
        std::array<uint32_t, 64> stateI{};
        std::array<float, 64> stateF{};
        void* userData = nullptr;
        double accumulatedActiveSeconds = 0.0;
        double activeClockStartSeconds = -1.0;
    };

    std::unordered_map<std::string, WorkspaceState> workspaces_;
    std::vector<std::string> workspaceOrder_;
    std::unordered_map<std::string, std::string> fileToWorkspace_;
    std::unordered_map<std::string, bool> workspaceDirty_;
    std::unordered_map<std::string, LanWorkspaceOffer> pendingLanOffersById_;

    std::unordered_map<std::string, std::string> latestSources_;
    // Per-workspace "earliest compile time" used for debounce/backoff scheduling.
    std::unordered_map<std::string, double> pendingCompilesAt_;
    std::unordered_map<std::string, double> ignoreWatcherUntil_;
    std::string activeWorkspaceName_;
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
        std::string workspaceName;
        std::string sourceName;
        std::string source;
        std::size_t sourceHash = 0;
    };

    struct CompileResult {
        std::string workspaceName;
        std::shared_ptr<JitProgram> program;
        std::size_t sourceHash = 0;
    };

    struct CompileFailureState {
        std::size_t sourceHash = 0;
        bool suppressionLogged = false;
    };
    struct InFlightStatus {
        bool inFlight = false;
        bool stalled = false;
        bool shouldReset = false;
    };

    std::mutex compileMutex_;
    std::condition_variable compileCv_;
    // Main thread enqueues compile jobs; compile thread pops and executes them.
    std::deque<CompileJob> compileJobs_;
    std::queue<CompileResult> compileResults_;
    std::unordered_map<std::string, std::size_t> inFlightSourceHashes_;
    double inFlightStartTime_ = 0.0;
    std::jthread compileThread_;
    std::shared_ptr<std::atomic<bool>> compileThreadRunning_;
    std::shared_ptr<std::atomic<bool>> compileThreadExited_;
    std::unordered_map<std::string, CompileFailureState> compileFailures_;
    std::unordered_map<std::string, double> compileRetryAfter_;
    std::unordered_map<std::string, std::deque<double>> recentErrors_;
    bool resetRequested_ = false;
    bool resetWaitLogged_ = false;

    bool RegisterWorkspace(const WorkspaceDescriptor& descriptor);
    void SyncWorkspaceUiState();
    bool CreateWorkspaceFromUI(const std::string& workspaceName);
    bool DeleteWorkspaceFromUI(const std::string& workspaceName);
    void SwitchToWorkspace(const std::string& workspaceName, bool focusCppDocument);
    bool ExportActiveWorkspace(const std::string& targetPath) const;
    bool ImportWorkspace(const std::string& sourcePath);
    bool InitLanShare();
    void UpdateLanShareUiState();
    void HandleShareWorkspaceRequest(const std::vector<std::string>& targetPeerIds, bool shareToAll);
    void HandleWorkspaceShareDecision(const std::string& offerId, bool accepted);
    bool ImportWorkspacePackage(const std::string& packageData, const std::string& sourceHint);
    std::string BuildCompileSourceForWorkspace(const std::string& workspaceName) const;
    void QueueCompileForWorkspace(const std::string& workspaceName, double nowSeconds, bool immediate);
    void UpdateWorkspaceSourceFromDocument(const std::string& workspaceName,
                                           const std::string& filepath,
                                           const std::string& content);
    std::string WorkspaceForPath(const std::string& filepath) const;
    bool ActiveWorkspaceHasCompileError() const;
    void SaveActiveWorkspaceRuntimeState();
    void LoadWorkspaceRuntimeState(const std::string& workspaceName);

    void ResetJIT();
    void CompletePendingJITReset();
    void OnFileChanged(const std::string& filepath);
    void ProcessPendingReloads(double nowSeconds);

    void HandleDocumentEdited(const std::string& filepath, const std::string& content);
    void HandleActiveDocumentChanged(const std::string& filepath, const std::string& content);
    void QueueCompile(const std::string& workspaceName, const std::string& source, double nowSeconds, bool immediate);
    std::vector<std::string> CollectDueCompiles(double nowSeconds) const;
    InFlightStatus EvaluateInFlightStatus(double nowSeconds, bool includeQueuedJobs);
    void EnqueueDueCompiles(const std::vector<std::string>& duePaths);
    void SubmitDueCompiles(double nowSeconds);
    void HandleCompileFailure(const CompileResult& result);
    void HandleCompileSuccess(const CompileResult& result);
    void ProcessCompileResults();
    void CompileThreadMain(std::shared_ptr<std::atomic<bool>> running);
    bool ActivateProgramForWorkspace(const std::string& workspaceName);
    void InitializeProgramIfNeeded(const std::shared_ptr<JitProgram>& program);
    void ShutdownProgramIfInitialized(const std::shared_ptr<JitProgram>& program);

    bool EnsureSceneRenderTarget(int width, int height);
    void DestroySceneRenderTarget();
    void RenderSceneToTexture();

    bool InitWindow();
    bool InitGL();
    bool InitUI();
    bool InitJIT();
    bool InitWatcher();

    unsigned int CreateShaderProgram(const char* vsSource, const char* fsSource);

    bool shutdown_ = false;
};
