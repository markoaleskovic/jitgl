#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <array>
#include <mutex>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Include this so std::unique_ptr knows how to delete it
#include "ui/EditorUI.h"
#include "runtime/EngineContext.h"
#include "jit/JitEngine.h"
#include "uniform/UniformControls.h"
#include "uniform/UniformRegistry.h"
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
    struct SharedTextureResource {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
    };

    struct PipelineConnectionBinding {
        std::string sourceWorkspace;
        std::string sourceSlot;
    };

    static constexpr std::size_t kWorkspaceStateBufferBytes = EngineContext::kStateBufferSize;
    static constexpr std::size_t kWorkspaceArenaBytes = 1024 * 1024;

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<EditorUI> ui_ = nullptr;

    std::shared_ptr<JitEngine>   jit_;
    std::unique_ptr<FileWatcher> watcher_;

    std::unique_ptr<ConsoleRedirectSession> consoleRedirect_;
    std::unique_ptr<WorkspaceManager> workspaceManager_;
    std::unique_ptr<LanWorkspaceShareService> lanShare_;
    bool networkEnabled_ = true;

    // Main-thread runtime state
    EngineContext ctx_;
    std::shared_ptr<JitProgram> activeProgram_;
    std::unordered_map<std::string, std::shared_ptr<JitProgram>> compiledPrograms_;
    struct WorkspaceState {
        std::string name;
        std::string directory;
        std::string cppPath;
        std::string shaderPath;
        std::string uniformsPath;
        std::string consoleLogPath;
        std::string engineLogPath;
        std::string cppSource;
        std::string shaderSource;
        UniformRegistry uniforms;
        std::array<uint32_t, 64> stateI{};
        std::array<float, 64> stateF{};
        std::array<unsigned char, kWorkspaceStateBufferBytes> stateBuffer{};
        void* userData = nullptr;
        std::uint64_t stateAbiHash = 0;
        std::vector<unsigned char> arenaStorage;
        std::size_t arenaOffset = 0;
        unsigned int vao = 0;
        unsigned int vbo = 0;
        std::vector<std::string> shaderDependencies;
        std::vector<std::string> samplerUniformNames;
        std::vector<std::string> storageBufferNames;
        std::string outputTextureName;
        bool pipelineEnabled = true;
        std::array<GLuint, 2> passFbos{};
        std::array<GLuint, 2> passColorTextures{};
        int passWidth = 0;
        int passHeight = 0;
        int passWriteIndex = 0;
        std::array<GLuint, 2> gpuQueries{};
        int gpuQueryWriteIndex = 0;
        std::array<bool, 2> gpuQueryPending{};
        float lastGpuPassTimeMs = 0.0f;
        GLuint samplerLocationProgram = 0;
        std::unordered_map<std::string, GLint> samplerLocationCache;
        GLuint globalUniformLocationProgram = 0;
        std::unordered_map<std::string, GLint> globalUniformLocationCache;
        double accumulatedActiveSeconds = 0.0;
        double activeClockStartSeconds = -1.0;
        bool playbackPaused = false;
        float playbackSpeed = 1.0f;
        bool loopEnabled = false;
        double loopStartSeconds = 0.0;
        double loopEndSeconds = 10.0;
        double timelineMaxSeconds = 30.0;
    };

    std::unordered_map<std::string, WorkspaceState> workspaces_;
    std::vector<std::string> workspaceOrder_;
    std::unordered_map<std::string, std::string> fileToWorkspace_;
    std::unordered_map<std::string, bool> workspaceDirty_;
    std::unordered_map<std::string, LanWorkspaceOffer> pendingLanOffersById_;

    std::unordered_map<std::string, std::string> latestSources_;
    std::unordered_map<std::string, std::size_t> latestSourceHashes_;
    std::unordered_map<std::string, std::vector<UniformDescriptor>> latestUniformDescriptors_;
    std::unordered_map<std::string, std::vector<std::string>> latestSamplerUniformNames_;
    std::unordered_map<std::string, std::vector<std::string>> latestStorageBufferNames_;
    std::unordered_map<std::string, std::vector<std::string>> latestShaderDependencies_;
    std::unordered_map<std::string, std::uint64_t> latestStateAbiHashes_;
    // Per-workspace "earliest compile time" used for debounce/backoff scheduling.
    std::unordered_map<std::string, double> pendingCompilesAt_;
    std::unordered_map<std::string, double> ignoreWatcherUntil_;
    std::string activeWorkspaceName_;
    std::string activeFilePath_;
    std::unordered_map<std::string, SharedTextureResource> sharedTextures_;
    std::unordered_map<std::string, std::unordered_map<std::string, PipelineConnectionBinding>> pipelineConnections_;
    std::vector<EditorUI::PipelineGlobalUniformView> pipelineGlobalUniforms_;
    std::unordered_map<std::string, double> dependencyFlashUntil_;
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
        std::uint64_t stateAbiHash = 0;
        std::vector<UniformDescriptor> uniformDescriptors;
        std::vector<std::string> samplerUniformNames;
        std::vector<std::string> storageBufferNames;
        std::vector<std::string> shaderDependencies;
    };

    struct CompileResult {
        std::string workspaceName;
        std::shared_ptr<JitProgram> program;
        std::size_t sourceHash = 0;
        std::uint64_t stateAbiHash = 0;
        std::vector<UniformDescriptor> uniformDescriptors;
        std::vector<std::string> samplerUniformNames;
        std::vector<std::string> storageBufferNames;
        std::vector<std::string> shaderDependencies;
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
    bool LoadShowcaseWorkspaceFromAssets(bool focusWorkspace);
    bool InitLanShare();
    void UpdateLanShareUiState();
    void HandleNetworkEnabledChanged(bool enabled);
    void HandleLoadShowcaseWorkspaceRequest();
    void HandleShareWorkspaceRequest(const std::vector<std::string>& targetPeerIds, bool shareToAll);
    void HandleWorkspaceShareDecision(const std::string& offerId, bool accepted);
    void HandleRequestFirewallAccess();
    void HandlePipelineMoveCommand(const std::string& workspaceName, int delta);
    void HandlePipelineAddPassCommand(const std::string& workspaceName);
    void HandlePipelineConfigChanged(const EditorUI::PipelineEditCommand& command);
    bool HandlePipelineSaveChainRequest(const std::string& path) const;
    void HandlePipelineResetRequest();
    void HandlePipelineOpenFileRequest(const std::string& workspaceName, bool openCppFile);
    void HandlePipelineConnectionCommand(const EditorUI::PipelineConnectionCommand& command);
    void HandlePipelineGlobalUniformCommand(const EditorUI::PipelineGlobalUniformCommand& command);
    bool ExportPipelineResource(const std::string& resourceName, const std::string& targetPath) const;
    bool ImportWorkspacePackage(const std::string& packageData, const std::string& sourceHint);
    bool BuildCompileSourceForWorkspace(const std::string& workspaceName,
                                        std::string* outSource,
                                        std::vector<UniformDescriptor>* outUniformDescriptors,
                                        std::vector<std::string>* outSamplerUniformNames,
                                        std::vector<std::string>* outStorageBufferNames,
                                        std::vector<std::string>* outShaderDependencies,
                                        std::string* outError) const;
    void QueueCompileForWorkspace(const std::string& workspaceName, double nowSeconds, bool immediate);
    void UpdateWorkspaceSourceFromDocument(const std::string& workspaceName,
                                           const std::string& filepath,
                                           const std::string& content);
    std::string WorkspaceForPath(const std::string& filepath) const;
    bool ActiveWorkspaceHasCompileError() const;
    void SaveWorkspaceRuntimeState(const std::string& workspaceName);
    void SaveActiveWorkspaceRuntimeState();
    void PersistWorkspaceUniformState(const std::string& workspaceName) const;
    void LoadWorkspaceRuntimeState(const std::string& workspaceName);
    void ResetWorkspaceRuntimeState(WorkspaceState* workspace, bool clearStateAbiHash);
    void HardResetActiveWorkspaceState(const std::string& reason, bool clearStateAbiHash);
    void HandleUniformEditCommand(const UniformEditCommand& command);
    std::string ActiveWorkspaceUniformStateJson() const;
    void HandlePlaybackCommand(const EditorUI::PlaybackCommand& command);
    void AdvanceWorkspacePlayback(WorkspaceState* workspace, double nowSeconds);
    void NormalizeWorkspacePlaybackRange(WorkspaceState* workspace);
    EditorUI::PlaybackState BuildPlaybackStateForWorkspace(const std::string& workspaceName) const;

    void ResetJIT();
    void CompletePendingJITReset();
    void OnFileChanged(const std::string& filepath);
    void ProcessPendingReloads(double nowSeconds);

    void HandleDocumentEdited(const std::string& filepath, const std::string& content);
    void HandleActiveDocumentChanged(const std::string& filepath, const std::string& content);
    void QueueCompile(const std::string& workspaceName,
                      const std::string& source,
                      std::uint64_t stateAbiHash,
                      std::vector<UniformDescriptor> uniformDescriptors,
                      std::vector<std::string> samplerUniformNames,
                      std::vector<std::string> storageBufferNames,
                      std::vector<std::string> shaderDependencies,
                      double nowSeconds,
                      bool immediate);
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
    bool EnsureWorkspaceGeometry(WorkspaceState* workspace);
    void ReleaseWorkspaceGeometry(WorkspaceState* workspace);
    bool EnsureWorkspacePassTargets(WorkspaceState* workspace, int width, int height);
    void ReleaseWorkspacePassTargets(WorkspaceState* workspace);
    void ActivateWorkspaceGeometry(const std::string& workspaceName);

    bool EnsureSceneRenderTarget(int width, int height);
    void DestroySceneRenderTarget();
    void ApplyWorkspaceUniforms(const std::string& workspaceName);
    void ApplyPipelineGlobalUniforms(WorkspaceState* workspace, GLuint programHandle);
    bool ResolvePipelineSamplerBinding(const WorkspaceState& workspace,
                                       const std::string& samplerName,
                                       std::string* outResourceName,
                                       bool* outIsExplicit) const;
    void BindSharedSamplerUniforms(WorkspaceState* workspace, GLuint programHandle);
    void UpdatePipelineUiState();
    void RenderSceneToTexture();

    bool InitWindow();
    bool InitGL();
    bool InitUI();
    bool InitJIT();
    bool InitWatcher();
    void ApplyGraphicsSettings(const EditorUI::AppSettings& settings);
    void FrameCapWait(double frameStartSeconds);

    bool vsyncEnabled_ = true;
    int  targetFramerate_ = 60;

    // Master toggle for forwarding keyboard/mouse to JIT code. Defaults off
    // so a freshly-launched editor never silently grabs input. F1 flips it
    // pre-snapshot so the user can always disable input even if a runaway
    // scene captures Escape.
    bool inputsEnabled_ = false;
    // Per-frame edge-detection state, lives in the host so hot-reloads do
    // not produce spurious press events for a key that was already held.
    std::array<std::uint8_t, InputState::kKeyCount> prevKeyDown_{};
    std::array<std::uint8_t, InputState::kMouseButtonCount> prevMouseDown_{};
    bool prevKeyStateValid_ = false;
    bool prevMouseButtonStateValid_ = false;
    bool inputHotkeyHeld_ = false;
    double prevMouseRawX_ = 0.0;
    double prevMouseRawY_ = 0.0;
    bool prevMousePosValid_ = false;
    float pendingScrollX_ = 0.0f;
    float pendingScrollY_ = 0.0f;
    static void GlfwScrollTrampoline(GLFWwindow* window, double xoffset, double yoffset);
    void HandleScrollEvent(double xoffset, double yoffset);
    void UpdateInputState();

    unsigned int CreateShaderProgram(const char* vsSource, const char* fsSource);

    bool shutdown_ = false;
};
