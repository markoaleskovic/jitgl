#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "TextEditor.h"
#include "imgui_markdown.h"
#include "system/AppPreferences.h"
#include "uniform/UniformControls.h"

struct GLFWwindow;

struct Document {
    std::string workspaceName;
    std::string filename;
    std::string filepath;
    TextEditor editor;
    bool isOpen = true;
    bool isDirty = false;
    double lastModifiedTime = 0.0;
    std::string lastKnownText;
};

class EditorUI {
public:
    struct MarkdownColors {
        ImVec4 h1, h2, h3, separator;
    };

    struct MdSection {
        std::string text;
        bool isCode;
        std::string lang;
    };

    struct NetworkPeer {
        std::string id;
        std::string displayName;
        std::string ipAddress;
    };

    struct IncomingWorkspaceShareOffer {
        std::string offerId;
        std::string workspaceName;
        std::string senderName;
    };

    struct NetworkDiagnostics {
        bool serviceRunning = false;
        bool udpSocketBound = false;
        bool tcpSocketBound = false;
        bool multicastJoinAttempted = false;
        bool multicastJoinSucceeded = false;
        bool winsockInitialized = false;

        uint16_t discoveryPort = 0;
        uint16_t transferPort = 0;

        std::string localPeerId;
        std::string localDisplayName;
        std::string discoveryMulticastAddress;
        std::string lastError;
        std::string lastUdpSenderIp;
        std::vector<std::string> localIpv4Addresses;
        std::vector<std::string> directedBroadcastAddresses;
        std::size_t unicastProbeTargetCount = 0;

        double nowSeconds = 0.0;
        double lastUdpSentSeconds = 0.0;
        double lastUdpReceivedSeconds = 0.0;
        double lastHelloSentSeconds = 0.0;
        double lastHelloReceivedSeconds = 0.0;

        std::uint64_t udpPacketsSent = 0;
        std::uint64_t udpPacketsSendFailed = 0;
        std::uint64_t udpPacketsReceived = 0;
        std::uint64_t helloSentCount = 0;
        std::uint64_t helloReceivedCount = 0;
        std::uint64_t offersSentCount = 0;
        std::uint64_t offersReceivedCount = 0;
        std::uint64_t outgoingFetchAttempts = 0;
        std::uint64_t outgoingFetchSuccesses = 0;
        std::uint64_t outgoingFetchFailures = 0;
        std::uint64_t incomingTransferRequests = 0;
        std::uint64_t incomingTransferSuccesses = 0;
        std::uint64_t incomingTransferFailures = 0;

        std::size_t peersKnown = 0;
        std::size_t pendingIncomingOffers = 0;
        std::size_t pendingOutgoingPackets = 0;
        std::size_t cachedSharedPayloads = 0;
    };

    enum class PlaybackCommandType : std::uint8_t {
        TogglePause,
        Rewind,
        SetTime,
        SetSpeed,
        SetLoopEnabled,
        SetLoopStart,
        SetLoopEnd,
        SetTimelineMax,
    };

    struct PlaybackCommand {
        PlaybackCommandType type = PlaybackCommandType::SetTime;
        float value = 0.0f;
        bool enabled = false;
    };

    struct PlaybackState {
        float currentTimeSeconds = 0.0f;
        float timelineMaxSeconds = 30.0f;
        bool paused = false;
        float speed = 1.0f;
        bool loopEnabled = false;
        float loopStartSeconds = 0.0f;
        float loopEndSeconds = 10.0f;
    };

    enum class RendererOutputMode : std::uint8_t {
        ActiveWorkspace,
        PipelineChain
    };

    enum class PipelineEditAction : std::uint8_t {
        SetOutputName,
        SetEnabled,
    };

    struct PipelineEditCommand {
        PipelineEditAction action = PipelineEditAction::SetOutputName;
        std::string workspaceName;
        std::string outputName;
        bool enabled = true;
    };

    enum class PipelinePassType : std::uint8_t {
        Raster,
        Compute,
        Hybrid,
    };

    struct PipelinePassView {
        std::string workspaceName;
        std::string outputName;
        std::string cppPath;
        std::string shaderPath;
        std::vector<std::string> inputSamplers;
        std::vector<std::string> inputBuffers;
        std::vector<std::string> outputTextures;
        std::vector<std::string> outputBuffers;
        bool enabled = true;
        bool active = false;
        bool compiled = false;
        bool hasCompileError = false;
        PipelinePassType passType = PipelinePassType::Raster;
        float gpuTimeMs = 0.0f;
    };

    struct PipelineResourceView {
        std::string name;
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
    };

    struct PipelineConnectionView {
        std::string sourceWorkspace;
        std::string sourceSlot;
        std::string targetWorkspace;
        std::string targetSlot;
        bool explicitConnection = false;
    };

    struct PipelineDependencyView {
        std::string path;
        std::vector<std::string> dependentWorkspaces;
        bool flashing = false;
    };

    enum class PipelineGlobalUniformType : std::uint8_t {
        Float,
        Int,
        Bool,
        Vec4,
    };

    struct PipelineGlobalUniformView {
        std::string name;
        PipelineGlobalUniformType type = PipelineGlobalUniformType::Float;
        float floatValue = 0.0f;
        int intValue = 0;
        bool boolValue = false;
        std::array<float, 4> vec4Value{ 0.0f, 0.0f, 0.0f, 0.0f };
    };

    struct PipelineConnectionCommand {
        std::string sourceWorkspace;
        std::string sourceSlot;
        std::string targetWorkspace;
        std::string targetSlot;
        bool clear = false;
    };

    struct PipelineGlobalUniformCommand {
        enum class Action : std::uint8_t {
            Upsert,
            Remove,
        };
        Action action = Action::Upsert;
        PipelineGlobalUniformView uniform;
    };

    EditorUI();
    ~EditorUI();

    void Init(GLFWwindow* window);
    void NewFrame();
    void Draw();
    void Render();
    void Shutdown();

    // Data injection methods
    void AddDocument(const std::string& workspaceName,
                     const std::string& filename,
                     const std::string& filepath,
                     const std::string& content);
    void UpdateDocumentContent(const std::string& filepath, const std::string& content);
    void SetActiveDocument(const std::string& filepath);
    void SetSaveCallback(std::function<bool(const std::string&, const std::string&)> cb);
    void SetDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb);
    void SetActiveDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb);
    void SetCreateWorkspaceCallback(std::function<void(const std::string&)> cb);
    void SetDeleteWorkspaceCallback(std::function<void(const std::string&)> cb);
    void SetWorkspaceSwitchedCallback(std::function<void(const std::string&)> cb);
    void SetWorkspaceLineAppendedCallback(std::function<void(const std::string&, const std::string&, bool)> cb);
    void SetExportWorkspaceCallback(std::function<bool(const std::string&)> cb);
    void SetImportWorkspaceCallback(std::function<bool(const std::string&)> cb);
    void SetShareWorkspaceCallback(std::function<void(const std::vector<std::string>&, bool)> cb);
    void SetWorkspaceShareDecisionCallback(std::function<void(const std::string&, bool)> cb);
    void SetRequestFirewallAccessCallback(std::function<void()> cb);
    void SetHardResetRuntimeCallback(std::function<void()> cb);
    void SetNetworkEnabledChangedCallback(std::function<void(bool)> cb);
    bool IsNetworkEnabled() const;
    void SetWorkspaces(const std::vector<std::string>& workspaceNames, const std::string& activeWorkspace);
    void SetActiveWorkspace(const std::string& workspaceName);
    void SetWorkspaceOutputHistory(const std::string& workspaceName,
                                   std::vector<std::string> consoleHistory,
                                   std::vector<std::string> logHistory);
    void SetNetworkPeers(std::vector<NetworkPeer> peers);
    void SetNetworkDiagnostics(NetworkDiagnostics diagnostics);
    void QueueIncomingWorkspaceShareOffer(IncomingWorkspaceShareOffer offer);
    void SetRendererTexture(unsigned int texture, int width, int height);
    RendererOutputMode GetRendererOutputMode() const;
    void SetCompilationStatus(bool isCompiling, bool hasError, bool isStalled = false);
    void SetUniformValues(std::vector<UniformValue> values);
    void SetUniformEditCallback(std::function<void(const UniformEditCommand&)> cb);
    void SetUniformJsonSnapshotCallback(std::function<std::string()> cb);
    void SetPlaybackState(PlaybackState state);
    void SetPlaybackCommandCallback(std::function<void(const PlaybackCommand&)> cb);
    void SetLoadShowcaseWorkspaceCallback(std::function<void()> cb);
    void SetPipelinePasses(std::vector<PipelinePassView> passes);
    void SetPipelineResources(std::vector<PipelineResourceView> resources);
    void SetPipelineConnections(std::vector<PipelineConnectionView> connections);
    void SetPipelineDependencies(std::vector<PipelineDependencyView> dependencies);
    void SetPipelineGlobalUniforms(std::vector<PipelineGlobalUniformView> globals);
    void SetPipelineMoveCallback(std::function<void(const std::string&, int)> cb);
    void SetPipelineEditCallback(std::function<void(const PipelineEditCommand&)> cb);
    void SetPipelineAddPassCallback(std::function<void(const std::string&)> cb);
    void SetPipelineSaveChainCallback(std::function<bool(const std::string&)> cb);
    void SetPipelineResetCallback(std::function<void()> cb);
    void SetPipelineOpenFileCallback(std::function<void(const std::string&, bool)> cb);
    void SetPipelineConnectionCallback(std::function<void(const PipelineConnectionCommand&)> cb);
    void SetPipelineGlobalUniformCallback(std::function<void(const PipelineGlobalUniformCommand&)> cb);
    void SetPipelineResourceExportCallback(std::function<bool(const std::string&, const std::string&)> cb);
    bool ShouldLoadShowcaseWorkspaceOnStartup() const;
    void SetupDarkTheme() const;
    void SetupLightTheme() const;

    void AddConsoleOutput(const std::string& text);
    void AddLogOutput(const std::string& text);

    static void MarkdownFormatCallback(const ImGui::MarkdownFormatInfo& info, bool start);

private:
    enum class UiTheme {
        Dark,
        Light
    };

    GLFWwindow* window;
    AppPreferences appPreferences_;
    std::vector<Document> openDocuments;
    std::unordered_map<std::string, std::vector<std::string>> workspaceConsoleLines_;
    std::unordered_map<std::string, std::vector<std::string>> workspaceLogLines_;
    std::vector<std::string> workspaceNames_;
    std::string activeWorkspaceName_;
    std::mutex consoleMutex;
    std::mutex logMutex;
    std::mutex workspaceMutex_;

    bool consoleScrollToBottom = true;
    bool logScrollToBottom = true;

    std::array<char, 512> commandBuffer{};
    std::vector<std::string> commandHistory;

    std::function<bool(const std::string&, const std::string&)> onSaveDocument_;
    std::function<void(const std::string&, const std::string&)> onDocumentChanged_;
    std::function<void(const std::string&, const std::string&)> onActiveDocumentChanged_;
    std::function<void(const std::string&)> onCreateWorkspace_;
    std::function<void(const std::string&)> onDeleteWorkspace_;
    std::function<void(const std::string&)> onWorkspaceSwitched_;
    std::function<void(const std::string&, const std::string&, bool)> onWorkspaceLineAppended_;
    std::function<bool(const std::string&)> onExportWorkspace_;
    std::function<bool(const std::string&)> onImportWorkspace_;
    std::function<void(const std::vector<std::string>&, bool)> onShareWorkspace_;
    std::function<void(const std::string&, bool)> onWorkspaceShareDecision_;
    std::function<void()> onRequestFirewallAccess_;
    std::function<void()> onHardResetRuntime_;
    std::function<void(bool)> onNetworkEnabledChanged_;
    bool networkEnabled_ = true;

    std::vector<NetworkPeer> networkPeers_;
    NetworkDiagnostics networkDiagnostics_;
    std::unordered_map<std::string, bool> selectedNetworkPeers_;
    std::vector<IncomingWorkspaceShareOffer> pendingWorkspaceShareOffers_;
    bool openShareWorkspacePopup_ = false;
    bool workspaceSharePromptOpen_ = false;
    bool showNetworkDiagnostics_ = false;
    bool openFirewallAccessPopup_ = false;

    std::string activeDocumentPath_;
    // Used once to force-select a tab after workspace/document changes.
    std::string pendingDocumentSelectionPath_;
    unsigned int rendererTexture_ = 0;
    int rendererTextureWidth_ = 0;
    int rendererTextureHeight_ = 0;
    RendererOutputMode rendererOutputMode_ = RendererOutputMode::PipelineChain;

    bool isCompiling_ = false;
    bool hasCompileError_ = false;
    bool isStalled_ = false;
    std::vector<UniformValue> uniformValues_;
    std::function<void(const UniformEditCommand&)> onUniformEdit_;
    std::function<std::string()> onUniformJsonSnapshot_;
    PlaybackState playbackState_{};
    std::function<void(const PlaybackCommand&)> onPlaybackCommand_;
    std::function<void()> onLoadShowcaseWorkspace_;
    std::vector<PipelinePassView> pipelinePasses_;
    std::vector<PipelineResourceView> pipelineResources_;
    std::vector<PipelineConnectionView> pipelineConnections_;
    std::vector<PipelineDependencyView> pipelineDependencies_;
    std::vector<PipelineGlobalUniformView> pipelineGlobalUniforms_;
    std::function<void(const std::string&, int)> onPipelineMove_;
    std::function<void(const PipelineEditCommand&)> onPipelineEdit_;
    std::function<void(const std::string&)> onPipelineAddPass_;
    std::function<bool(const std::string&)> onPipelineSaveChain_;
    std::function<void()> onPipelineReset_;
    std::function<void(const std::string&, bool)> onPipelineOpenFile_;
    std::function<void(const PipelineConnectionCommand&)> onPipelineConnection_;
    std::function<void(const PipelineGlobalUniformCommand&)> onPipelineGlobalUniform_;
    std::function<bool(const std::string&, const std::string&)> onPipelineResourceExport_;
    void* pipelineNodeEditorConfig_ = nullptr;
    void* pipelineNodeEditorContext_ = nullptr;
    std::unordered_set<std::string> pipelinePositionedNodes_;
    std::unordered_map<std::string, std::uint64_t> pipelineStableEditorIds_;
    std::uint64_t pipelineNextStableEditorId_ = 1;
    bool pipelineNavigateToContent_ = true;
    bool showGlobalUniformsWindow_ = false;
    int selectedPipelineAddPassIndex_ = 0;
    bool openCreateWorkspacePopup_ = false;
    std::array<char, 128> newWorkspaceNameBuffer_{};

    void SetupDockspace();
    void DrawMenuBar();
    void DrawConsolePane();
    void DrawTextEditorPane();
    void DrawFileMenu(const std::vector<std::string>& workspaceNamesSnapshot,
                      bool canDeleteAnyWorkspace,
                      std::string* pendingWorkspaceDelete);
    void DrawWorkspaceMenu(const std::vector<std::string>& workspaceNamesSnapshot,
                           std::string* pendingWorkspaceSwitch) const;
    void DrawViewMenu();
    void DrawHelpMenu();
    void OpenCreateWorkspacePopupIfRequested();
    void DrawCreateWorkspacePopup();
    void OpenShareWorkspacePopupIfRequested();
    void DrawShareWorkspacePopup();
    void DrawIncomingWorkspaceSharePopup();
    void DrawNetworkDiagnosticsWindow();
    void DrawMenuWorkspaceLabel(const std::vector<std::string>& workspaceNamesSnapshot);
    void ApplyPendingWorkspaceAction(const std::string& pendingWorkspaceDelete,
                                     const std::string& pendingWorkspaceSwitch);
    void DrawCompileStatusIndicator() const;
    void DrawWorkspaceSidebar(const std::vector<std::string>& workspaceNamesSnapshot,
                              bool canDeleteAnyWorkspace,
                              std::string* pendingWorkspaceSwitch,
                              std::string* pendingWorkspaceDelete);
    void DrawEditorTabsArea();
    bool DrawEditorTab(Document& doc,
                       double currentTime,
                       bool* pendingSelectionVisible,
                       bool* pendingSelectionConsumed);
    void AutosaveDirtyDocuments(double currentTime);
    void DrawRendererTab();
    void DrawPipelineTab();
    void DrawPlaybackTransportBar();
    void DrawUniformsTab();
    void DrawConsoleTab(const std::string& currentWorkspace);
    void DrawLogsTab(const std::string& currentWorkspace);
    std::string ResolveCurrentWorkspaceName();
    void DrawWelcomePopup();
    void DrawShowcaseGuidePopup();
    void DrawRuntimeGuidePopup();
    void DrawMarkdown(const std::string& markdown, bool lightTheme);
    void LoadMarkdownFiles();
    void LoadWelcomePreference();
    void SaveWelcomePreference();
    void LoadShowcasePreference();
    void SaveShowcasePreference();
    void LoadNetworkPreference();
    void SaveNetworkPreference();
    void ApplyThemeAndScale(float dpiScale, bool recreateFontTexture);
    void ToggleTheme();
    void ApplyEditorPalette(Document& doc) const;
    bool IsLightTheme() const;
    void DrawRendererFullscreen();
    std::uint64_t AcquirePipelineEditorStableId(const std::string& key);

    float currentDpiScale_ = 1.0f;
    float pendingDpiScale_ = 1.0f;
    double lastDpiApplyTime_ = -1.0;
    void ReloadFontAtlas(float dpiScale, bool recreateTexture);
    void ApplyPendingDpiScale();
    void SetDpiScale(float newScale);
    void HandleGlobalShortcuts();
    bool IsWorkspaceNumberShortcutHeld(std::size_t index, bool canUseCtrlShortcuts) const;
    void TriggerChordAction(bool held, bool* chordState, const std::function<void()>& action);
    void ToggleActiveWorkspaceDocument();
    void CycleWorkspace(int direction);
    void ActivateWorkspaceByIndex(std::size_t index);
    bool ctrlWorkspaceCycleChordHeld_ = false;
    // Edge-triggered key chord latches to prevent repeat-firing while keys are held down.
    std::array<bool, 10> ctrlWorkspaceIndexChordHeld_{};
    bool ctrlTabChordHeld_ = false;
    bool ctrlPlusChordHeld_ = false;
    bool ctrlMinusChordHeld_ = false;
    bool ctrlNewWorkspaceChordHeld_ = false;
    bool ctrlThemeToggleChordHeld_ = false;
    bool ctrlRenderModeChordHeld_ = false;
    bool ctrlFullscreenChordHeld_ = false;
    bool showWelcomeOnStartup_ = true;
    bool welcomePopupOpenedThisSession_ = false;
    bool openWelcomePopupRequested_ = false;
    bool doNotShowWelcomeAgain_ = false;
    bool openRuntimeGuidePopupRequested_ = false;
    bool runtimeGuidePopupOpenedThisSession_ = false;
    bool loadShowcaseWorkspaceOnStartup_ = true;
    bool openShowcaseGuidePopupRequested_ = false;
    bool showcaseGuidePopupOpenedThisSession_ = false;
    bool disableShowcaseStartupFromGuide_ = false;
    bool focusCreateWorkspaceNameInput_ = false;
    UiTheme currentTheme_ = UiTheme::Dark;
    bool themeApplyPending_ = false;
    bool showUniformControlsPanel_ = false;
    bool showPlaybackControlsPanel_ = false;
    bool rendererTabActive_ = false;
    bool rendererFullscreen_ = false;
    int focusEditorRequestFramesRemaining_ = 0;

    std::string welcomeMarkdown_;
    std::string showcaseGuideMarkdown_;
    std::string guideMarkdown_;
    ImGui::MarkdownConfig markdownConfig_;

    ImFont* fontH1_ = nullptr;
    ImFont* fontH2_ = nullptr;
    ImFont* fontH3_ = nullptr;

    std::vector<MdSection> ParseMarkdownSections(const std::string& src);

    MarkdownColors mdColors_;
    void SetMarkdownColorsForTheme(bool lightTheme);

    bool shutdown_ = false;
    bool initialized_ = false;
};
