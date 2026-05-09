#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <functional>
#include <unordered_map>
#include "TextEditor.h"
#include "imgui_markdown.h"

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
    void SetWorkspaces(const std::vector<std::string>& workspaceNames, const std::string& activeWorkspace);
    void SetActiveWorkspace(const std::string& workspaceName);
    void SetWorkspaceOutputHistory(const std::string& workspaceName,
                                   std::vector<std::string> consoleHistory,
                                   std::vector<std::string> logHistory);
    void SetNetworkPeers(std::vector<NetworkPeer> peers);
    void SetNetworkDiagnostics(NetworkDiagnostics diagnostics);
    void QueueIncomingWorkspaceShareOffer(IncomingWorkspaceShareOffer offer);
    void SetRendererTexture(unsigned int texture, int width, int height);
    void SetCompilationStatus(bool isCompiling, bool hasError, bool isStalled = false);
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

    bool isCompiling_ = false;
    bool hasCompileError_ = false;
    bool isStalled_ = false;
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
    void DrawConsoleTab(const std::string& currentWorkspace);
    void DrawLogsTab(const std::string& currentWorkspace);
    std::string ResolveCurrentWorkspaceName();
    void DrawWelcomePopup();
    void DrawRuntimeGuidePopup();
    void DrawMarkdown(const std::string& markdown, bool lightTheme);
    void LoadMarkdownFiles();
    void LoadWelcomePreference();
    void SaveWelcomePreference() const;
    void ApplyThemeAndScale(float dpiScale, bool recreateFontTexture);
    void ToggleTheme();
    void ApplyEditorPalette(Document& doc) const;
    bool IsLightTheme() const;
    void DrawRendererFullscreen();

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
    bool ctrlFullscreenChordHeld_ = false;
    bool showWelcomeOnStartup_ = true;
    bool welcomePopupOpenedThisSession_ = false;
    bool openWelcomePopupRequested_ = false;
    bool doNotShowWelcomeAgain_ = false;
    bool openRuntimeGuidePopupRequested_ = false;
    bool runtimeGuidePopupOpenedThisSession_ = false;
    bool focusCreateWorkspaceNameInput_ = false;
    UiTheme currentTheme_ = UiTheme::Dark;
    bool themeApplyPending_ = false;
    bool rendererFullscreen_ = false;
    int focusEditorRequestFramesRemaining_ = 0;

    std::string welcomeMarkdown_;
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
