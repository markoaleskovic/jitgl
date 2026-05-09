#include "ui/EditorUI.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <string>
#include <imgui_internal.h>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <ranges>
#include <sstream>
#include <nfd.hpp>

namespace {
    constexpr double AUTOSAVE_DEBOUNCE_SECONDS = 0.05;
    constexpr const char* GLSL_VERSION = "#version 330 core";
    constexpr std::size_t MAX_CONSOLE_LINES = 2000;
    constexpr std::size_t MAX_LOG_LINES = 2000;
    constexpr const char* FONT_PATH = "assets/JetBrainsMono-Regular.ttf";
    constexpr float BASE_FONT_SIZE = 16.0f;
    constexpr const char* FALLBACK_WORKSPACE_NAME = "default";
    constexpr const char* WELCOME_PREFS_FILE = ".jitgl_welcome_prefs";
    constexpr double DPI_APPLY_MIN_INTERVAL_SECONDS = 0.08;

    const ImVec4 kEditorPaneBgColor = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    const ImVec4 kPanelBgColor = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    const ImVec4 kUtilityPaneBgColor = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    const ImVec4 kUtilityPaneChildBgColor = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    const ImVec4 kLightEditorPaneBgColor = ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    const ImVec4 kLightPanelBgColor = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    const ImVec4 kLightUtilityPaneBgColor = ImVec4(0.89f, 0.89f, 0.89f, 1.00f);
    const ImVec4 kLightUtilityPaneChildBgColor = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);

    bool IsKeyDown(GLFWwindow* window, int key) {
        return window != nullptr && glfwGetKey(window, key) == GLFW_PRESS;
    }

    std::string BuildWorkspaceDisplayLabel(const std::string& workspaceName, std::size_t index) {
        return std::to_string(index + 1) + ": " + workspaceName;
    }
}

// style start

void EditorUI::SetupDarkTheme() const {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImVec4* colors = style.Colors;

    // Flat UI, minimal rounding
    style.WindowRounding    = 2.0f;
    style.ChildRounding     = 2.0f;
    style.FrameRounding     = 2.0f;
    style.PopupRounding     = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.TabRounding       = 2.0f;

    // Subtle borders
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.ScrollbarSize     = 14.0f;
    
    // Padding
    style.FramePadding      = ImVec2(4.0f, 4.0f);
    style.WindowPadding     = ImVec2(8.0f, 8.0f);
    style.ItemSpacing       = ImVec2(8.0f, 4.0f);

    // Color Palette (VS Code / Dark+ inspired)
    const ImVec4 bgColor         = kEditorPaneBgColor;
    const ImVec4 panelBgColor    = kPanelBgColor;
    const ImVec4 borderColor     = ImVec4(0.20f, 0.20f, 0.20f, 1.00f); // #333333
    const ImVec4 highlightColor  = ImVec4(0.00f, 0.47f, 0.83f, 1.00f); // #007ACC (Blue accent)
    const ImVec4 textColor       = ImVec4(0.80f, 0.80f, 0.80f, 1.00f); // #CCCCCC
    
    colors[ImGuiCol_Text]                   = textColor;
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    
    colors[ImGuiCol_WindowBg]               = bgColor;
    colors[ImGuiCol_ChildBg]                = bgColor;
    colors[ImGuiCol_PopupBg]                = panelBgColor;
    
    colors[ImGuiCol_Border]                 = borderColor;
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    colors[ImGuiCol_FrameBg]                = panelBgColor;
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    
    colors[ImGuiCol_TitleBg]                = panelBgColor;
    colors[ImGuiCol_TitleBgActive]          = panelBgColor;
    colors[ImGuiCol_TitleBgCollapsed]       = panelBgColor;
    
    colors[ImGuiCol_MenuBarBg]              = panelBgColor;
    
    colors[ImGuiCol_ScrollbarBg]            = bgColor;
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    
    colors[ImGuiCol_CheckMark]              = highlightColor;
    colors[ImGuiCol_SliderGrab]             = highlightColor;
    colors[ImGuiCol_SliderGrabActive]       = highlightColor;
    
    colors[ImGuiCol_Button]                 = panelBgColor;
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = highlightColor;
    
    colors[ImGuiCol_Header]                 = panelBgColor;
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = highlightColor;
    
    colors[ImGuiCol_Separator]              = borderColor;
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    
    colors[ImGuiCol_ResizeGrip]             = ImVec4(1.00f, 1.00f, 1.00f, 0.05f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    colors[ImGuiCol_ResizeGripActive]       = highlightColor;
    
    colors[ImGuiCol_Tab]                    = panelBgColor;
    colors[ImGuiCol_TabHovered]             = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TabActive]              = bgColor; // Active tab merges into background
    colors[ImGuiCol_TabUnfocused]           = panelBgColor;
    colors[ImGuiCol_TabUnfocusedActive]     = bgColor;
    
    colors[ImGuiCol_DockingPreview]         = ImVec4(highlightColor.x, highlightColor.y, highlightColor.z, 0.40f);
    colors[ImGuiCol_DockingEmptyBg]         = bgColor;
}

void EditorUI::SetupLightTheme() const {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 2.0f;
    style.ChildRounding     = 2.0f;
    style.FrameRounding     = 2.0f;
    style.PopupRounding     = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.TabRounding       = 2.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.ScrollbarSize     = 14.0f;

    style.FramePadding      = ImVec2(4.0f, 4.0f);
    style.WindowPadding     = ImVec2(8.0f, 8.0f);
    style.ItemSpacing       = ImVec2(8.0f, 4.0f);

    const ImVec4 bgColor         = kLightEditorPaneBgColor;
    const ImVec4 panelBgColor    = kLightPanelBgColor;
    const ImVec4 borderColor     = ImVec4(0.75f, 0.75f, 0.75f, 1.00f);
    const ImVec4 highlightColor  = ImVec4(0.07f, 0.45f, 0.88f, 1.00f);
    const ImVec4 textColor       = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);

    colors[ImGuiCol_Text]                   = textColor;
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);

    colors[ImGuiCol_WindowBg]               = bgColor;
    colors[ImGuiCol_ChildBg]                = bgColor;
    colors[ImGuiCol_PopupBg]                = panelBgColor;

    colors[ImGuiCol_Border]                 = borderColor;
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]                = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);

    colors[ImGuiCol_TitleBg]                = panelBgColor;
    colors[ImGuiCol_TitleBgActive]          = panelBgColor;
    colors[ImGuiCol_TitleBgCollapsed]       = panelBgColor;

    colors[ImGuiCol_MenuBarBg]              = panelBgColor;

    colors[ImGuiCol_ScrollbarBg]            = bgColor;
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.72f, 0.72f, 0.72f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.62f, 0.62f, 0.62f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);

    colors[ImGuiCol_CheckMark]              = highlightColor;
    colors[ImGuiCol_SliderGrab]             = highlightColor;
    colors[ImGuiCol_SliderGrabActive]       = highlightColor;

    colors[ImGuiCol_Button]                 = ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.89f, 0.89f, 0.89f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.84f, 0.84f, 0.84f, 1.00f);

    colors[ImGuiCol_Header]                 = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = highlightColor;

    colors[ImGuiCol_Separator]              = borderColor;
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);

    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.35f, 0.35f, 0.35f, 0.10f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.35f, 0.35f, 0.35f, 0.20f);
    colors[ImGuiCol_ResizeGripActive]       = highlightColor;

    colors[ImGuiCol_Tab]                    = panelBgColor;
    colors[ImGuiCol_TabHovered]             = ImVec4(0.87f, 0.87f, 0.87f, 1.00f);
    colors[ImGuiCol_TabActive]              = bgColor;
    colors[ImGuiCol_TabUnfocused]           = panelBgColor;
    colors[ImGuiCol_TabUnfocusedActive]     = bgColor;

    colors[ImGuiCol_DockingPreview]         = ImVec4(highlightColor.x, highlightColor.y, highlightColor.z, 0.30f);
    colors[ImGuiCol_DockingEmptyBg]         = bgColor;
}

// style end

EditorUI::EditorUI() : window(nullptr), activeWorkspaceName_(FALLBACK_WORKSPACE_NAME) {
    markdownConfig_.linkCallback = [](ImGui::MarkdownLinkCallbackData data) {
        // No links for now, but required by API
    };
    markdownConfig_.tooltipCallback = nullptr;
    markdownConfig_.imageCallback = nullptr;
    markdownConfig_.userData = this;
    markdownConfig_.formatFlags = ImGuiMarkdownFormatFlags_GithubStyle;
    markdownConfig_.formatCallback = MarkdownFormatCallback;
}

EditorUI::~EditorUI() {
    Shutdown();
}

void EditorUI::LoadWelcomePreference() {
    showWelcomeOnStartup_ = true;

    std::ifstream inFile(WELCOME_PREFS_FILE, std::ios::binary);
    if (!inFile.is_open()) {
        return;
    }

    std::string value;
    std::getline(inFile, value);
    if (value == "hide=1") {
        showWelcomeOnStartup_ = false;
    }
}

void EditorUI::SaveWelcomePreference() const {
    std::ofstream outFile(WELCOME_PREFS_FILE, std::ios::trunc | std::ios::binary);
    if (!outFile.is_open()) {
        return;
    }

    outFile << (showWelcomeOnStartup_ ? "hide=0\n" : "hide=1\n");
}

void EditorUI::ApplyEditorPalette(Document& doc) const {
    if (currentTheme_ == UiTheme::Light) {
        auto palette = TextEditor::GetLightPalette();
        palette[(int)TextEditor::PaletteIndex::Background] = 0xffececec;
        doc.editor.SetPalette(palette);
        return;
    }

    auto palette = TextEditor::GetDarkPalette();
    palette[(int)TextEditor::PaletteIndex::Background] = 0xff1e1e1e;
    doc.editor.SetPalette(palette);
}

bool EditorUI::IsLightTheme() const {
    return currentTheme_ == UiTheme::Light;
}

void EditorUI::ApplyThemeAndScale(float dpiScale, bool recreateFontTexture) {
    if (currentTheme_ == UiTheme::Light) {
        SetupLightTheme();
    } else {
        SetupDarkTheme();
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    style.ScaleAllSizes(dpiScale);
    if (style.ScrollbarSize <= 0.0f || !std::isfinite(style.ScrollbarSize)) {
        style.ScrollbarSize = 1.0f;
    }

    ReloadFontAtlas(dpiScale, recreateFontTexture);
}

void EditorUI::ToggleTheme() {
    currentTheme_ = (currentTheme_ == UiTheme::Dark) ? UiTheme::Light : UiTheme::Dark;
    for (auto& doc : openDocuments) {
        ApplyEditorPalette(doc);
    }
    themeApplyPending_ = true;
}

void EditorUI::ReloadFontAtlas(float dpiScale, bool recreateTexture) {
    ImGuiIO& io = ImGui::GetIO();

    if (recreateTexture) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
    }

    io.Fonts->Clear();

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = true;

    const float requestedSize = BASE_FONT_SIZE * std::max(0.5f, dpiScale);
    const float pixelSize = std::round(requestedSize);

    ImFont* font = io.Fonts->AddFontFromFileTTF(FONT_PATH, pixelSize, &fontConfig);
    if (!font) {
        font = io.Fonts->AddFontDefault();
    }

    fontH1_ = io.Fonts->AddFontFromFileTTF(FONT_PATH, pixelSize * 1.5f, &fontConfig);
    fontH2_ = io.Fonts->AddFontFromFileTTF(FONT_PATH, pixelSize * 1.3f, &fontConfig);
    fontH3_ = io.Fonts->AddFontFromFileTTF(FONT_PATH, pixelSize * 1.1f, &fontConfig);

    io.FontDefault = font;
    io.FontGlobalScale = 1.0f;
    io.Fonts->Build();

    if (recreateTexture) {
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
}

void EditorUI::Init(GLFWwindow *win) {
    window = win;
    LoadMarkdownFiles();

    NFD_Init();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Disable Dear ImGui's default Ctrl+Tab/Super+Tab window switcher so Ctrl+Tab can switch editor files.
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context != nullptr) {
        context->ConfigNavWindowingKeyNext = ImGuiKey_None;
        context->ConfigNavWindowingKeyPrev = ImGuiKey_None;
    }

    float contentScaleX = 1.0f;
    float contentScaleY = 1.0f;
    glfwGetWindowContentScale(window, &contentScaleX, &contentScaleY);
    if (const float monitorScale = 0.5f * (contentScaleX + contentScaleY);
        std::isfinite(monitorScale) && monitorScale > 0.0f) {
        currentDpiScale_ = std::clamp(monitorScale, 0.5f, 3.0f);
        pendingDpiScale_ = currentDpiScale_;
    }

    ApplyThemeAndScale(currentDpiScale_, false);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);

    LoadWelcomePreference();
    openWelcomePopupRequested_ = showWelcomeOnStartup_;
    welcomePopupOpenedThisSession_ = false;
    doNotShowWelcomeAgain_ = false;

    initialized_ = true;
    shutdown_ = false;
}

void EditorUI::ApplyPendingDpiScale() {
    if (const bool dpiChanged = std::abs(pendingDpiScale_ - currentDpiScale_) >= 0.001f;
        !dpiChanged && !themeApplyPending_) {
        return;
    }

    const double now = ImGui::GetTime();
    if (lastDpiApplyTime_ >= 0.0 && (now - lastDpiApplyTime_) < DPI_APPLY_MIN_INTERVAL_SECONDS) {
        return;
    }

    if (!std::isfinite(pendingDpiScale_)) {
        pendingDpiScale_ = currentDpiScale_;
        return;
    }

    if (pendingDpiScale_ <= 0.0f) {
        pendingDpiScale_ = currentDpiScale_;
        return;
    }

    ApplyThemeAndScale(pendingDpiScale_, true);
    currentDpiScale_ = pendingDpiScale_;
    themeApplyPending_ = false;
    lastDpiApplyTime_ = now;
}

void EditorUI::NewFrame() {
    ApplyPendingDpiScale();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EditorUI::Draw() {
    HandleGlobalShortcuts();

    if (rendererFullscreen_) {
        DrawRendererFullscreen();
    } else {
        DrawMenuBar();
        SetupDockspace();
        DrawTextEditorPane();
        DrawConsolePane();
    }

    DrawWelcomePopup();
    DrawRuntimeGuidePopup();
}

void EditorUI::SetupDockspace() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpaceWindow", nullptr, window_flags);
    ImGui::PopStyleVar();

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    static bool layout_initialized = false;
    if (!layout_initialized) {
        layout_initialized = true;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_editor_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.5f, nullptr, &dock_main_id);

        ImGui::DockBuilderDockWindow("Code Editor", dock_editor_id);
        ImGui::DockBuilderDockWindow("Utility View", dock_main_id);
        ImGui::DockBuilderFinish(dockspace_id);
    }
    ImGui::End();
}

void EditorUI::AddDocument(const std::string& workspaceName,
                           const std::string& filename,
                           const std::string& filepath,
                           const std::string& content) {
    auto existing = std::find_if(openDocuments.begin(), openDocuments.end(), [&](const Document& doc) {
        return doc.filepath == filepath;
    });
    if (existing != openDocuments.end()) {
        existing->workspaceName = workspaceName;
        existing->filename = filename;
        existing->editor.SetText(content);
        existing->lastKnownText = content;
        existing->isDirty = false;
        return;
    }

    Document doc;
    doc.workspaceName = workspaceName;
    doc.filename = filename;
    doc.filepath = filepath;
    doc.editor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    ApplyEditorPalette(doc);
    doc.editor.SetText(content);
    doc.lastKnownText = content;
    doc.isDirty = false;
    doc.isOpen = true;
    openDocuments.push_back(doc);

    if (activeDocumentPath_.empty() && workspaceName == activeWorkspaceName_) {
        activeDocumentPath_ = filepath;
    }
}

void EditorUI::UpdateDocumentContent(const std::string& filepath, const std::string& content) {
    auto existing = std::find_if(openDocuments.begin(), openDocuments.end(), [&](const Document& doc) {
        return doc.filepath == filepath;
    });
    if (existing == openDocuments.end()) {
        return;
    }

    existing->editor.SetText(content);
    existing->lastKnownText = content;
    existing->isDirty = false;
}

void EditorUI::SetActiveDocument(const std::string& filepath) {
    activeDocumentPath_ = filepath;
    pendingDocumentSelectionPath_ = filepath;
}

void EditorUI::SetSaveCallback(std::function<bool(const std::string&, const std::string&)> cb) {
    onSaveDocument_ = std::move(cb);
}

void EditorUI::SetDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    onDocumentChanged_ = std::move(cb);
}

void EditorUI::SetActiveDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    onActiveDocumentChanged_ = std::move(cb);
}

void EditorUI::SetCreateWorkspaceCallback(std::function<void(const std::string&)> cb) {
    onCreateWorkspace_ = std::move(cb);
}

void EditorUI::SetDeleteWorkspaceCallback(std::function<void(const std::string&)> cb) {
    onDeleteWorkspace_ = std::move(cb);
}

void EditorUI::SetWorkspaceSwitchedCallback(std::function<void(const std::string&)> cb) {
    onWorkspaceSwitched_ = std::move(cb);
}

void EditorUI::SetWorkspaceLineAppendedCallback(std::function<void(const std::string&, const std::string&, bool)> cb) {
    onWorkspaceLineAppended_ = std::move(cb);
}

void EditorUI::SetExportWorkspaceCallback(std::function<bool(const std::string&)> cb) {
    onExportWorkspace_ = std::move(cb);
}

void EditorUI::SetImportWorkspaceCallback(std::function<bool(const std::string&)> cb) {
    onImportWorkspace_ = std::move(cb);
}

void EditorUI::SetWorkspaces(const std::vector<std::string>& workspaceNames, const std::string& activeWorkspace) {
    workspaceNames_ = workspaceNames;

    std::erase_if(openDocuments,
                  [&](const Document& doc) {
                      return std::ranges::find(workspaceNames_, doc.workspaceName) == workspaceNames_.end();
                  });

    {
        std::scoped_lock lock(consoleMutex);
        std::erase_if(workspaceConsoleLines_,
                      [&](const auto& entry) {
                          return std::ranges::find(workspaceNames_, entry.first) == workspaceNames_.end();
                      });
    }
    {
        std::scoped_lock lock(logMutex);
        std::erase_if(workspaceLogLines_,
                      [&](const auto& entry) {
                          return std::ranges::find(workspaceNames_, entry.first) == workspaceNames_.end();
                      });
    }

    SetActiveWorkspace(activeWorkspace);
}

void EditorUI::SetActiveWorkspace(const std::string& workspaceName) {
    if (workspaceName.empty()) {
        return;
    }

    {
        std::scoped_lock lock(workspaceMutex_);
        activeWorkspaceName_ = workspaceName;
    }

    auto docIt = std::find_if(openDocuments.begin(), openDocuments.end(), [&](const Document& doc) {
        return doc.workspaceName == workspaceName && doc.filepath == activeDocumentPath_;
    });
    if (docIt == openDocuments.end()) {
        auto firstWorkspaceDoc = std::find_if(openDocuments.begin(), openDocuments.end(), [&](const Document& doc) {
            return doc.workspaceName == workspaceName;
        });
        if (firstWorkspaceDoc != openDocuments.end()) {
            activeDocumentPath_ = firstWorkspaceDoc->filepath;
            pendingDocumentSelectionPath_ = activeDocumentPath_;
        }
    }

    consoleScrollToBottom = true;
    logScrollToBottom = true;
}

void EditorUI::SetWorkspaceOutputHistory(const std::string& workspaceName,
                                         std::vector<std::string> consoleHistory,
                                         std::vector<std::string> logHistory) {
    if (workspaceName.empty()) {
        return;
    }

    if (consoleHistory.size() > MAX_CONSOLE_LINES) {
        const std::size_t removeCount = consoleHistory.size() - MAX_CONSOLE_LINES;
        consoleHistory.erase(consoleHistory.begin(),
                             consoleHistory.begin() + static_cast<std::ptrdiff_t>(removeCount));
    }
    if (logHistory.size() > MAX_LOG_LINES) {
        const std::size_t removeCount = logHistory.size() - MAX_LOG_LINES;
        logHistory.erase(logHistory.begin(),
                         logHistory.begin() + static_cast<std::ptrdiff_t>(removeCount));
    }

    {
        std::scoped_lock lock(consoleMutex);
        workspaceConsoleLines_[workspaceName] = std::move(consoleHistory);
    }
    {
        std::scoped_lock lock(logMutex);
        workspaceLogLines_[workspaceName] = std::move(logHistory);
    }
}

void EditorUI::SetRendererTexture(unsigned int texture, int width, int height) {
    rendererTexture_ = texture;
    rendererTextureWidth_ = width;
    rendererTextureHeight_ = height;
}
void EditorUI::SetCompilationStatus(bool isCompiling, bool hasError, bool isStalled) {
    isCompiling_ = isCompiling;
    hasCompileError_ = hasError;
    isStalled_ = isStalled;
}

void EditorUI::SetDpiScale(float newScale) {
    // Clamp the scale to prevent the UI from becoming unreadably small or impossibly large
    if (newScale < 0.5f) newScale = 0.5f;
    if (newScale > 3.0f) newScale = 3.0f;

    if (std::abs(newScale - pendingDpiScale_) < 0.001f) {
        return;
    }
    pendingDpiScale_ = newScale;
}

void EditorUI::DrawWelcomePopup() {
    if (openWelcomePopupRequested_ && !welcomePopupOpenedThisSession_) {
        doNotShowWelcomeAgain_ = !showWelcomeOnStartup_;
        ImGui::OpenPopup("Welcome to JITGL");
        welcomePopupOpenedThisSession_ = true;
    }

    if (welcomePopupOpenedThisSession_ && !ImGui::IsPopupOpen("Welcome to JITGL")) {
        openWelcomePopupRequested_ = false;
        welcomePopupOpenedThisSession_ = false;
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(860.0f, 680.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 520.0f),
                                        ImVec2(viewport->WorkSize.x * 0.96f, viewport->WorkSize.y * 0.96f));

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
    if (!ImGui::BeginPopupModal("Welcome to JITGL", nullptr, popupFlags)) {
        return;
    }

    const float footerReserve = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    if (ImGui::BeginChild("WelcomeScrollableContent", ImVec2(0.0f, -footerReserve), false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        DrawMarkdown(welcomeMarkdown_, IsLightTheme());
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Checkbox("Do not show this again", &doNotShowWelcomeAgain_);

    const float startButtonWidth = 90.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - startButtonWidth);
    if (ImGui::Button("Start", ImVec2(startButtonWidth, 0.0f))) {
        showWelcomeOnStartup_ = !doNotShowWelcomeAgain_;
        SaveWelcomePreference();
        openWelcomePopupRequested_ = false;
        welcomePopupOpenedThisSession_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawRuntimeGuidePopup() {
    if (openRuntimeGuidePopupRequested_ && !runtimeGuidePopupOpenedThisSession_) {
        ImGui::OpenPopup("Runtime State Guide");
        runtimeGuidePopupOpenedThisSession_ = true;
    }

    if (runtimeGuidePopupOpenedThisSession_ && !ImGui::IsPopupOpen("Runtime State Guide")) {
        openRuntimeGuidePopupRequested_ = false;
        runtimeGuidePopupOpenedThisSession_ = false;
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 720.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f, 560.0f),
                                        ImVec2(viewport->WorkSize.x * 0.96f, viewport->WorkSize.y * 0.96f));

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
    if (!ImGui::BeginPopupModal("Runtime State Guide", nullptr, popupFlags)) {
        return;
    }

    const float footerReserve = ImGui::GetFrameHeightWithSpacing() * 2.1f;
    if (ImGui::BeginChild("RuntimeGuideScrollableContent", ImVec2(0.0f, -footerReserve), false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        DrawMarkdown(guideMarkdown_, IsLightTheme());
    }
    ImGui::EndChild();

    ImGui::Separator();
    const float closeButtonWidth = 90.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeButtonWidth);
    if (ImGui::Button("Close", ImVec2(closeButtonWidth, 0.0f))) {
        openRuntimeGuidePopupRequested_ = false;
        runtimeGuidePopupOpenedThisSession_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawMarkdown(const std::string& markdown, bool lightTheme) {
    SetMarkdownColorsForTheme(lightTheme);

    const ImVec4 codeBg = lightTheme ? ImVec4(0.88f, 0.89f, 0.91f, 1.0f)
                                     : ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
    const ImVec4 codeText = lightTheme ? ImVec4(0.13f, 0.43f, 0.13f, 1.0f)
                                       : ImVec4(0.56f, 0.86f, 0.56f, 1.0f);

    markdownConfig_.headingFormats[0] = { fontH1_, true };
    markdownConfig_.headingFormats[1] = { fontH2_, true };
    markdownConfig_.headingFormats[2] = { fontH3_, false };

    auto sections = ParseMarkdownSections(markdown);

    int codeChildId = 0;

    for (const auto& sec : sections) {
        if (!sec.isCode) {
            ImGui::Markdown(sec.text.c_str(), sec.text.size(), markdownConfig_);
        } else {
            // Measure height: one line per \n
            int lines = 1;
            for (char c : sec.text) if (c == '\n') ++lines;
            float h = ImGui::GetTextLineHeight() * lines
                    + ImGui::GetStyle().WindowPadding.y * 2.0f;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, codeBg);
            ImGui::PushStyleColor(ImGuiCol_Text,    codeText);
            std::string childId = "##code_" + std::to_string(codeChildId++);
            if (ImGui::BeginChild(childId.c_str(), ImVec2(0.0f, h), true,
                                  ImGuiWindowFlags_NoScrollbar)) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(sec.text.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::Spacing();
        }
    }
}

std::vector<EditorUI::MdSection> EditorUI::ParseMarkdownSections(const std::string& src) {
    std::vector<MdSection> out;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t fence = src.find("\n```", pos);
        if (fence == std::string::npos) {
            out.push_back({ src.substr(pos), false, "" });
            break;
        }
        // prose before fence
        if (fence > pos)
            out.push_back({ src.substr(pos, fence - pos), false, "" });
        // find lang tag on same line
        size_t lineEnd = src.find('\n', fence + 4);
        std::string lang = (lineEnd != std::string::npos)
            ? src.substr(fence + 4, lineEnd - (fence + 4)) : "";
        // find closing fence
        size_t closePos = (lineEnd != std::string::npos)
            ? src.find("\n```", lineEnd) : std::string::npos;
        if (closePos == std::string::npos) break;
        std::string code = src.substr(lineEnd + 1, closePos - (lineEnd + 1));
        out.push_back({ code, true, lang });
        pos = closePos + 4; // skip past closing ```
        // skip newline after closing fence
        if (pos < src.size() && src[pos] == '\n') ++pos;
    }
    return out;
}

void EditorUI::SetMarkdownColorsForTheme(bool lightTheme) {
    mdColors_ = {
        // H1 — blue title
        lightTheme ? ImVec4(0.10f, 0.39f, 0.78f, 1.0f) : ImVec4(0.54f, 0.86f, 1.0f, 1.0f),
        // H2 — amber section
        lightTheme ? ImVec4(0.58f, 0.42f, 0.16f, 1.0f) : ImVec4(0.94f, 0.75f, 0.37f, 1.0f),
        // H3 — same as H2 but slightly muted
        lightTheme ? ImVec4(0.50f, 0.36f, 0.14f, 1.0f) : ImVec4(0.85f, 0.68f, 0.32f, 1.0f),
        // separator tint
        lightTheme ? ImVec4(0.70f, 0.70f, 0.70f, 1.0f) : ImVec4(0.35f, 0.35f, 0.35f, 1.0f),
    };
}

void EditorUI::MarkdownFormatCallback(const ImGui::MarkdownFormatInfo& info, bool start) {
    ImGui::defaultMarkdownFormatCallback(info, start);
    if (info.type == ImGui::MarkdownFormatType::HEADING) {
        EditorUI* ui = static_cast<EditorUI*>(info.config->userData);
        ImVec4 color = (info.level == 1) ? ui->mdColors_.h1
                     : (info.level == 2) ? ui->mdColors_.h2
                                         : ui->mdColors_.h3;
        if (start) ImGui::PushStyleColor(ImGuiCol_Text, color);
        else        ImGui::PopStyleColor();
    }
}

void EditorUI::LoadMarkdownFiles() {
    auto loadFile = [](const std::string& path) -> std::string {
        std::ifstream file(path);
        if (!file.is_open()) return "Failed to load: " + path;
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    };

    welcomeMarkdown_ = loadFile("assets/welcome.md");
    guideMarkdown_ = loadFile("assets/guide.md");
}

void EditorUI::TriggerChordAction(bool held, bool* chordState, const std::function<void()>& action) {
    // Fire once on key-down transition, not every frame while key is held.
    if (held && !(*chordState)) {
        action();
    }
    *chordState = held;
}

bool EditorUI::IsWorkspaceNumberShortcutHeld(std::size_t index, bool canUseCtrlShortcuts) const {
    if (!canUseCtrlShortcuts || index >= ctrlWorkspaceIndexChordHeld_.size()) {
        return false;
    }

    static constexpr std::array<std::array<int, 2>, 10> kShortcutKeys{{
        {{GLFW_KEY_1, GLFW_KEY_KP_1}},
        {{GLFW_KEY_2, GLFW_KEY_KP_2}},
        {{GLFW_KEY_3, GLFW_KEY_KP_3}},
        {{GLFW_KEY_4, GLFW_KEY_KP_4}},
        {{GLFW_KEY_5, GLFW_KEY_KP_5}},
        {{GLFW_KEY_6, GLFW_KEY_KP_6}},
        {{GLFW_KEY_7, GLFW_KEY_KP_7}},
        {{GLFW_KEY_8, GLFW_KEY_KP_8}},
        {{GLFW_KEY_9, GLFW_KEY_KP_9}},
        {{GLFW_KEY_0, GLFW_KEY_KP_0}},
    }};

    const auto& keys = kShortcutKeys[index];
    return IsKeyDown(window, keys[0]) || IsKeyDown(window, keys[1]);
}

void EditorUI::HandleGlobalShortcuts() {
    const bool ctrlHeld = IsKeyDown(window, GLFW_KEY_LEFT_CONTROL) || IsKeyDown(window, GLFW_KEY_RIGHT_CONTROL);
    const bool superHeld = IsKeyDown(window, GLFW_KEY_LEFT_SUPER) || IsKeyDown(window, GLFW_KEY_RIGHT_SUPER);
    const bool canUseCtrlShortcuts = ctrlHeld && !superHeld;

    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_GRAVE_ACCENT),
                       &ctrlWorkspaceCycleChordHeld_,
                       [this]() { CycleWorkspace(1); });

    for (std::size_t i = 0; i < ctrlWorkspaceIndexChordHeld_.size(); ++i) {
        TriggerChordAction(IsWorkspaceNumberShortcutHeld(i, canUseCtrlShortcuts),
                           &ctrlWorkspaceIndexChordHeld_[i],
                           [this, i]() { ActivateWorkspaceByIndex(i); });
    }

    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_TAB),
                       &ctrlTabChordHeld_,
                       [this]() { ToggleActiveWorkspaceDocument(); });
    TriggerChordAction(canUseCtrlShortcuts &&
                           (IsKeyDown(window, GLFW_KEY_EQUAL) || IsKeyDown(window, GLFW_KEY_KP_ADD)),
                       &ctrlPlusChordHeld_,
                       [this]() { SetDpiScale(currentDpiScale_ + 0.1f); });
    TriggerChordAction(canUseCtrlShortcuts &&
                           (IsKeyDown(window, GLFW_KEY_MINUS) || IsKeyDown(window, GLFW_KEY_KP_SUBTRACT)),
                       &ctrlMinusChordHeld_,
                       [this]() { SetDpiScale(currentDpiScale_ - 0.1f); });
    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_N),
                       &ctrlNewWorkspaceChordHeld_,
                       [this]() { openCreateWorkspacePopup_ = true; });
    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_T),
                       &ctrlThemeToggleChordHeld_,
                       [this]() { ToggleTheme(); });
    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_F),
                       &ctrlFullscreenChordHeld_,
                       [this]() {
                           rendererFullscreen_ = !rendererFullscreen_;
                           if (!rendererFullscreen_) {
                               focusEditorRequestFramesRemaining_ = 4;
                               if (!activeDocumentPath_.empty()) {
                                   pendingDocumentSelectionPath_ = activeDocumentPath_;
                               }
                           }
                       });
}

void EditorUI::ToggleActiveWorkspaceDocument() {
    if (activeWorkspaceName_.empty()) {
        return;
    }

    std::vector<std::size_t> activeWorkspaceDocIndices;
    activeWorkspaceDocIndices.reserve(openDocuments.size());
    for (std::size_t i = 0; i < openDocuments.size(); ++i) {
        const Document& doc = openDocuments[i];
        if (!doc.isOpen || doc.workspaceName != activeWorkspaceName_) {
            continue;
        }
        activeWorkspaceDocIndices.push_back(i);
    }

    if (activeWorkspaceDocIndices.empty()) {
        return;
    }

    std::size_t targetDocListIndex = 0;
    auto activeDocIt = std::find_if(activeWorkspaceDocIndices.begin(),
                                    activeWorkspaceDocIndices.end(),
                                    [&](std::size_t docIndex) {
                                        return openDocuments[docIndex].filepath == activeDocumentPath_;
                                    });
    if (activeDocIt != activeWorkspaceDocIndices.end()) {
        const auto currentPosition = static_cast<std::size_t>(
            std::distance(activeWorkspaceDocIndices.begin(), activeDocIt));
        targetDocListIndex = (currentPosition + 1) % activeWorkspaceDocIndices.size();
    }

    const Document& targetDocument = openDocuments[activeWorkspaceDocIndices[targetDocListIndex]];
    if (targetDocument.filepath == activeDocumentPath_) {
        return;
    }

    activeDocumentPath_ = targetDocument.filepath;
    pendingDocumentSelectionPath_ = targetDocument.filepath;
    if (onActiveDocumentChanged_) {
        onActiveDocumentChanged_(targetDocument.filepath, targetDocument.lastKnownText);
    }
}

void EditorUI::CycleWorkspace(int direction) {
    if (workspaceNames_.empty()) {
        return;
    }

    auto activeIt = std::find(workspaceNames_.begin(), workspaceNames_.end(), activeWorkspaceName_);
    std::size_t activeIndex = 0;
    if (activeIt != workspaceNames_.end()) {
        activeIndex = static_cast<std::size_t>(std::distance(workspaceNames_.begin(), activeIt));
    }

    const int workspaceCount = static_cast<int>(workspaceNames_.size());
    int nextIndex = static_cast<int>(activeIndex);
    nextIndex = (nextIndex + direction) % workspaceCount;
    if (nextIndex < 0) {
        nextIndex += workspaceCount;
    }

    const auto nextWorkspaceIndex = static_cast<std::size_t>(nextIndex);
    if (workspaceNames_[nextWorkspaceIndex] == activeWorkspaceName_) {
        return;
    }

    ActivateWorkspaceByIndex(nextWorkspaceIndex);
}

void EditorUI::ActivateWorkspaceByIndex(std::size_t index) {
    if (index >= workspaceNames_.size()) {
        return;
    }

    const std::string& workspaceName = workspaceNames_[index];
    if (workspaceName == activeWorkspaceName_) {
        return;
    }

    SetActiveWorkspace(workspaceName);
    if (onWorkspaceSwitched_) {
        onWorkspaceSwitched_(workspaceName);
    }
}

void EditorUI::DrawFileMenu(const std::vector<std::string>& workspaceNamesSnapshot,
                            bool canDeleteAnyWorkspace,
                            std::string* pendingWorkspaceDelete) {
    if (!ImGui::BeginMenu("File")) {
        return;
    }

    if (ImGui::MenuItem("New Workspace...", "Ctrl+N")) {
        openCreateWorkspacePopup_ = true;
    }

    if (ImGui::BeginMenu("Delete Workspace")) {
        if (!canDeleteAnyWorkspace) {
            ImGui::BeginDisabled();
        }
        for (std::size_t i = 0; i < workspaceNamesSnapshot.size(); ++i) {
            const std::string& workspaceName = workspaceNamesSnapshot[i];
            const std::string workspaceLabel = BuildWorkspaceDisplayLabel(workspaceName, i);
            if (ImGui::MenuItem(workspaceLabel.c_str())) {
                *pendingWorkspaceDelete = workspaceName;
            }
        }
        if (!canDeleteAnyWorkspace) {
            ImGui::EndDisabled();
        }
        ImGui::EndMenu();
    }

    (void)ImGui::MenuItem("Save", "Ctrl+S");
    ImGui::Separator();
    if (ImGui::MenuItem("Export Workspace...")) {
        const std::string currentWorkspace = ResolveCurrentWorkspaceName();
        const std::string defaultExportFilename = currentWorkspace + ".jws";
        nfdchar_t *outPath = nullptr;
        nfdfilteritem_t filterItem[1] = {{"JITGL Workspace", "jws,jitglws"}};
        nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, 1, nullptr, defaultExportFilename.c_str());

        if (result == NFD_OKAY) {
            bool exported = false;
            if (onExportWorkspace_) {
                exported = onExportWorkspace_(outPath);
            }
            if (exported) {
                AddLogOutput("[Workspace] Exported '" + currentWorkspace + "'.");
            } else {
                AddLogOutput("[Workspace Error] Failed to export '" + currentWorkspace + "'.");
            }
            NFD_FreePath(outPath);
        }
    }
    if (ImGui::MenuItem("Import Workspace...")) {
        nfdchar_t *outPath = nullptr;
        nfdfilteritem_t filterItem[1] = {{"JITGL Workspace", "jws,jitglws"}};
        nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 1, nullptr);

        if (result == NFD_OKAY) {
            bool imported = false;
            if (onImportWorkspace_) {
                imported = onImportWorkspace_(outPath);
            }
            if (imported) {
                AddLogOutput("[Workspace] Imported workspace package.");
            } else {
                AddLogOutput("[Workspace Error] Failed to import workspace package.");
            }
            NFD_FreePath(outPath);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    ImGui::EndMenu();
}

void EditorUI::DrawWorkspaceMenu(const std::vector<std::string>& workspaceNamesSnapshot,
                                 std::string* pendingWorkspaceSwitch) const {
    if (!ImGui::BeginMenu("Workspace")) {
        return;
    }

    for (std::size_t i = 0; i < workspaceNamesSnapshot.size(); ++i) {
        const std::string& workspaceName = workspaceNamesSnapshot[i];
        const std::string workspaceLabel = BuildWorkspaceDisplayLabel(workspaceName, i);
        const bool selected = (workspaceName == activeWorkspaceName_);
        if (ImGui::MenuItem(workspaceLabel.c_str(), nullptr, selected)) {
            *pendingWorkspaceSwitch = workspaceName;
        }
    }
    ImGui::EndMenu();
}

void EditorUI::DrawViewMenu() {
    if (!ImGui::BeginMenu("View")) {
        return;
    }

    if (ImGui::MenuItem("Increase DPI", "Ctrl++")) {
        SetDpiScale(currentDpiScale_ + 0.1f);
    }
    if (ImGui::MenuItem("Decrease DPI", "Ctrl+-")) {
        SetDpiScale(currentDpiScale_ - 0.1f);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset DPI")) {
        SetDpiScale(1.0f);
    }
    if (ImGui::MenuItem(IsLightTheme() ? "Switch to Dark Theme" : "Switch to Light Theme", "Ctrl+T")) {
        ToggleTheme();
    }
    ImGui::EndMenu();
}

void EditorUI::DrawHelpMenu() {
    if (!ImGui::BeginMenu("Help")) {
        return;
    }

    if (ImGui::MenuItem("Getting Started")) {
        openWelcomePopupRequested_ = true;
        welcomePopupOpenedThisSession_ = false;
    }
    if (ImGui::MenuItem("Runtime State Guide")) {
        openRuntimeGuidePopupRequested_ = true;
        runtimeGuidePopupOpenedThisSession_ = false;
    }
    ImGui::EndMenu();
}

void EditorUI::OpenCreateWorkspacePopupIfRequested() {
    if (!openCreateWorkspacePopup_) {
        return;
    }

    openCreateWorkspacePopup_ = false;
    newWorkspaceNameBuffer_[0] = '\0';
    focusCreateWorkspaceNameInput_ = true;
    ImGui::OpenPopup("Create Workspace");
}

void EditorUI::DrawCreateWorkspacePopup() {
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const ImGuiWindowFlags createWorkspacePopupFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                                       ImGuiWindowFlags_NoTitleBar |
                                                       ImGuiWindowFlags_NoResize |
                                                       ImGuiWindowFlags_NoMove;
    if (!ImGui::BeginPopupModal("Create Workspace", nullptr, createWorkspacePopupFlags)) {
        return;
    }

    // Keep close/reset behavior identical for Escape, Cancel, and successful Create.
    const auto closePopup = [this]() {
        newWorkspaceNameBuffer_[0] = '\0';
        focusCreateWorkspaceNameInput_ = false;
        ImGui::CloseCurrentPopup();
    };

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        closePopup();
        ImGui::EndPopup();
        return;
    }

    if (focusCreateWorkspaceNameInput_) {
        ImGui::SetKeyboardFocusHere();
    }

    const bool submitFromEnter = ImGui::InputText("Name",
                                                  newWorkspaceNameBuffer_.data(),
                                                  newWorkspaceNameBuffer_.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    focusCreateWorkspaceNameInput_ = false;

    const bool canCreate = newWorkspaceNameBuffer_[0] != '\0';
    bool createRequested = canCreate && submitFromEnter;

    if (!canCreate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Create")) {
        createRequested = true;
    }
    if (!canCreate) {
        ImGui::EndDisabled();
    }
    if (createRequested) {
        if (onCreateWorkspace_) {
            onCreateWorkspace_(newWorkspaceNameBuffer_.data());
        }
        closePopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        closePopup();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawMenuWorkspaceLabel(const std::vector<std::string>& workspaceNamesSnapshot) {
    std::string currentWorkspace;
    {
        std::scoped_lock lock(workspaceMutex_);
        currentWorkspace = activeWorkspaceName_;
    }
    if (currentWorkspace.empty()) {
        return;
    }

    std::string workspaceLabel = "Workspace: " + currentWorkspace;
    if (auto activeWorkspaceIt = std::ranges::find(workspaceNamesSnapshot, currentWorkspace);
        activeWorkspaceIt != workspaceNamesSnapshot.end()) {
        auto activeWorkspaceIndex = static_cast<std::size_t>(
            std::distance(workspaceNamesSnapshot.begin(), activeWorkspaceIt));
        workspaceLabel = "Workspace " + std::to_string(activeWorkspaceIndex + 1) + ": " + currentWorkspace;
    }

    const float centeredX = (ImGui::GetWindowWidth() - ImGui::CalcTextSize(workspaceLabel.c_str()).x) * 0.5f;
    if (centeredX > 0.0f) {
        ImGui::SetCursorPosX(centeredX);
    }

    const ImVec4 workspaceLabelColor = IsLightTheme() ? ImVec4(0.20f, 0.20f, 0.20f, 1.0f)
                                                       : ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
    ImGui::TextColored(workspaceLabelColor, "%s", workspaceLabel.c_str());
}

void EditorUI::ApplyPendingWorkspaceAction(const std::string& pendingWorkspaceDelete,
                                           const std::string& pendingWorkspaceSwitch) {
    // Deletion wins over switching when both are requested in the same frame.
    if (!pendingWorkspaceDelete.empty() && onDeleteWorkspace_) {
        onDeleteWorkspace_(pendingWorkspaceDelete);
        return;
    }

    if (pendingWorkspaceSwitch.empty()) {
        return;
    }

    SetActiveWorkspace(pendingWorkspaceSwitch);
    if (onWorkspaceSwitched_) {
        onWorkspaceSwitched_(pendingWorkspaceSwitch);
    }
}

void EditorUI::DrawCompileStatusIndicator() const {
    const ImVec4 stalledColor = IsLightTheme() ? ImVec4(0.78f, 0.33f, 0.00f, 1.0f)
                                               : ImVec4(1.0f, 0.4f, 0.0f, 1.0f);
    const ImVec4 compilingColor = IsLightTheme() ? ImVec4(0.72f, 0.56f, 0.00f, 1.0f)
                                                 : ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
    const ImVec4 errorColor = IsLightTheme() ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f)
                                             : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    const ImVec4 readyColor = IsLightTheme() ? ImVec4(0.16f, 0.56f, 0.24f, 1.0f)
                                             : ImVec4(0.4f, 1.0f, 0.4f, 1.0f);

    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 170.0f);
    if (isStalled_) {
        ImGui::TextColored(stalledColor, "STALLED?");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("JIT compilation is taking longer than expected.\nCheck Logs for details.");
        }
        return;
    }

    if (isCompiling_) {
        ImGui::TextColored(compilingColor, "COMPILING...");
        return;
    }

    if (hasCompileError_) {
        ImGui::TextColored(errorColor, "ERROR");
        return;
    }

    ImGui::TextColored(readyColor, "READY");
}

void EditorUI::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    const std::vector<std::string> workspaceNamesSnapshot = workspaceNames_;
    const bool canDeleteAnyWorkspace = workspaceNamesSnapshot.size() > 1;
    std::string pendingWorkspaceSwitch;
    std::string pendingWorkspaceDelete;

    DrawFileMenu(workspaceNamesSnapshot, canDeleteAnyWorkspace, &pendingWorkspaceDelete);
    DrawWorkspaceMenu(workspaceNamesSnapshot, &pendingWorkspaceSwitch);
    DrawViewMenu();
    DrawHelpMenu();
    OpenCreateWorkspacePopupIfRequested();
    DrawCreateWorkspacePopup();

    DrawMenuWorkspaceLabel(workspaceNamesSnapshot);
    ApplyPendingWorkspaceAction(pendingWorkspaceDelete, pendingWorkspaceSwitch);
    DrawCompileStatusIndicator();

    ImGui::EndMainMenuBar();
}

void EditorUI::DrawWorkspaceSidebar(const std::vector<std::string>& workspaceNamesSnapshot,
                                    bool canDeleteAnyWorkspace,
                                    std::string* pendingWorkspaceSwitch,
                                    std::string* pendingWorkspaceDelete) {
    constexpr float workspaceSidebarWidth = 165.0f;
    ImGui::BeginChild("WorkspaceSidebar", ImVec2(workspaceSidebarWidth, 0.0f), true);
    ImGui::TextUnformatted("Workspaces");
    ImGui::Separator();

    for (std::size_t i = 0; i < workspaceNamesSnapshot.size(); ++i) {
        const std::string& workspaceName = workspaceNamesSnapshot[i];
        const std::string workspaceLabel = BuildWorkspaceDisplayLabel(workspaceName, i);
        ImGui::PushID(workspaceName.c_str());

        const bool selected = (workspaceName == activeWorkspaceName_);
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const float deleteButtonWidth = canDeleteAnyWorkspace ? (ImGui::GetFrameHeight() - 2.0f) : 0.0f;
        const float selectableWidth = canDeleteAnyWorkspace
                                          ? std::max(1.0f, rowWidth - deleteButtonWidth - 6.0f)
                                          : rowWidth;
        if (ImGui::Selectable(workspaceLabel.c_str(), selected, 0, ImVec2(selectableWidth, 0.0f))) {
            *pendingWorkspaceSwitch = workspaceName;
        }

        if (ImGui::BeginPopupContextItem("WorkspaceSidebarContext")) {
            if (canDeleteAnyWorkspace) {
                if (ImGui::MenuItem("Delete Workspace")) {
                    *pendingWorkspaceDelete = workspaceName;
                }
            } else {
                ImGui::TextDisabled("Cannot delete last workspace");
            }
            ImGui::EndPopup();
        }

        if (canDeleteAnyWorkspace) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                *pendingWorkspaceDelete = workspaceName;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Delete workspace");
            }
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}

bool EditorUI::DrawEditorTab(Document& doc,
                             double currentTime,
                             bool* pendingSelectionVisible,
                             bool* pendingSelectionConsumed) {
    if (!doc.isOpen || doc.workspaceName != activeWorkspaceName_) {
        return false;
    }

    ImGuiTabItemFlags tabFlags = doc.isDirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
    if (!pendingDocumentSelectionPath_.empty() && doc.filepath == pendingDocumentSelectionPath_) {
        *pendingSelectionVisible = true;
        tabFlags |= ImGuiTabItemFlags_SetSelected;
    }

    if (!ImGui::BeginTabItem(doc.filename.c_str(), &doc.isOpen, tabFlags)) {
        return true;
    }

    if (focusEditorRequestFramesRemaining_ > 0 && doc.filepath == activeDocumentPath_) {
        // We set the focus to the next item (the child window inside TextEditor::Render)
        ImGui::SetKeyboardFocusHere();
    }

    if (!pendingDocumentSelectionPath_.empty() && doc.filepath == pendingDocumentSelectionPath_) {
        *pendingSelectionConsumed = true;
    }
    if (activeDocumentPath_ != doc.filepath) {
        activeDocumentPath_ = doc.filepath;
        if (onActiveDocumentChanged_) {
            onActiveDocumentChanged_(doc.filepath, doc.lastKnownText);
        }
    }

    doc.editor.Render(doc.filename.c_str());
    if (focusEditorRequestFramesRemaining_ > 0 && doc.filepath == activeDocumentPath_) {
        --focusEditorRequestFramesRemaining_;
    }

    std::string currentText = doc.editor.GetText();
    if (currentText != doc.lastKnownText) {
        doc.isDirty = true;
        doc.lastModifiedTime = currentTime;
        doc.lastKnownText = std::move(currentText);
        if (onDocumentChanged_) {
            onDocumentChanged_(doc.filepath, doc.lastKnownText);
        }
    }

    ImGui::EndTabItem();
    return true;
}

void EditorUI::AutosaveDirtyDocuments(double currentTime) {
    for (auto& doc : openDocuments) {
        if (!doc.isDirty || (currentTime - doc.lastModifiedTime) < AUTOSAVE_DEBOUNCE_SECONDS) {
            continue;
        }

        bool saved = false;
        if (onSaveDocument_) {
            saved = onSaveDocument_(doc.filepath, doc.lastKnownText);
        }

        if (saved) {
            doc.isDirty = false;
            AddLogOutput("[AutoSave] " + doc.filename + " committed to disk.");
        } else {
            AddLogOutput("[AutoSave Error] Failed to write " + doc.filepath);
        }
    }
}

void EditorUI::DrawEditorTabsArea() {
    ImGui::BeginChild("WorkspaceEditorArea", ImVec2(0.0f, 0.0f), false);
    if (ImGui::BeginTabBar("EditorTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        const double currentTime = ImGui::GetTime();
        bool drewAnyDocument = false;
        bool pendingSelectionVisible = false;
        bool pendingSelectionConsumed = false;

        for (auto& doc : openDocuments) {
            drewAnyDocument = DrawEditorTab(doc, currentTime, &pendingSelectionVisible, &pendingSelectionConsumed) ||
                              drewAnyDocument;
        }

        // Clear one-shot tab selection request once target is selected or no longer visible.
        if (!pendingDocumentSelectionPath_.empty() && (pendingSelectionConsumed || !pendingSelectionVisible)) {
            pendingDocumentSelectionPath_.clear();
        }

        AutosaveDirtyDocuments(currentTime);

        if (!drewAnyDocument) {
            ImGui::TextUnformatted("No files in this workspace.");
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void EditorUI::DrawTextEditorPane() {
    if (focusEditorRequestFramesRemaining_ > 0) {
        ImGui::SetNextWindowFocus();
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("Code Editor", nullptr, flags);

    const std::vector<std::string> workspaceNamesSnapshot = workspaceNames_;
    const bool canDeleteAnyWorkspace = workspaceNamesSnapshot.size() > 1;
    std::string pendingWorkspaceSwitch;
    std::string pendingWorkspaceDelete;

    DrawWorkspaceSidebar(workspaceNamesSnapshot,
                         canDeleteAnyWorkspace,
                         &pendingWorkspaceSwitch,
                         &pendingWorkspaceDelete);
    ApplyPendingWorkspaceAction(pendingWorkspaceDelete, pendingWorkspaceSwitch);

    ImGui::SameLine();
    DrawEditorTabsArea();
    ImGui::End();
}
void EditorUI::AddConsoleOutput(const std::string &text) {
    std::string workspaceName;
    {
        std::scoped_lock workspaceLock(workspaceMutex_);
        workspaceName = activeWorkspaceName_;
    }
    if (workspaceName.empty()) {
        workspaceName = FALLBACK_WORKSPACE_NAME;
    }

    {
        std::scoped_lock lock(consoleMutex);
        auto& lines = workspaceConsoleLines_[workspaceName];
        // Keep bounded history per workspace to avoid unbounded memory growth.
        lines.push_back(text);
        if (lines.size() > MAX_CONSOLE_LINES) {
            const std::size_t removeCount = lines.size() - MAX_CONSOLE_LINES;
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(removeCount));
        }
    }
    consoleScrollToBottom = true;

    if (onWorkspaceLineAppended_) {
        onWorkspaceLineAppended_(workspaceName, text, true);
    }
}

void EditorUI::AddLogOutput(const std::string &text) {
    std::string workspaceName;
    {
        std::scoped_lock workspaceLock(workspaceMutex_);
        workspaceName = activeWorkspaceName_;
    }
    if (workspaceName.empty()) {
        workspaceName = FALLBACK_WORKSPACE_NAME;
    }

    {
        std::scoped_lock lock(logMutex);
        auto& lines = workspaceLogLines_[workspaceName];
        lines.push_back(text);
        if (lines.size() > MAX_LOG_LINES) {
            const std::size_t removeCount = lines.size() - MAX_LOG_LINES;
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(removeCount));
        }
    }
    logScrollToBottom = true;

    if (onWorkspaceLineAppended_) {
        onWorkspaceLineAppended_(workspaceName, text, false);
    }
}

std::string EditorUI::ResolveCurrentWorkspaceName() {
    std::string currentWorkspace;
    {
        std::scoped_lock workspaceLock(workspaceMutex_);
        currentWorkspace = activeWorkspaceName_;
    }
    if (currentWorkspace.empty()) {
        currentWorkspace = FALLBACK_WORKSPACE_NAME;
    }
    return currentWorkspace;
}

void EditorUI::DrawRendererFullscreen() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    // Use a background color consistent with the theme
    const bool lightTheme = IsLightTheme();
    const ImVec4 bg = lightTheme ? kLightUtilityPaneBgColor : kUtilityPaneBgColor;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);

    if (ImGui::Begin("FullscreenRenderer", nullptr, flags)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            rendererFullscreen_ = false;
            focusEditorRequestFramesRemaining_ = 4;
            if (!activeDocumentPath_.empty()) {
                pendingDocumentSelectionPath_ = activeDocumentPath_;
            }
            ImGui::End();
            ImGui::PopStyleColor();
            return;
        }

        if (rendererTexture_ != 0 && rendererTextureWidth_ > 0 && rendererTextureHeight_ > 0) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float srcAspect = static_cast<float>(rendererTextureWidth_) / static_cast<float>(rendererTextureHeight_);
            ImVec2 imageSize = avail;

            if (imageSize.x > 0.0f && imageSize.y > 0.0f) {
                const float dstAspect = imageSize.x / imageSize.y;
                if (dstAspect > srcAspect) {
                    imageSize.x = imageSize.y * srcAspect;
                } else {
                    imageSize.y = imageSize.x / srcAspect;
                }
            }

            const ImVec2 cursor = ImGui::GetCursorPos();
            const float offsetX = (avail.x - imageSize.x) * 0.5f;
            const float offsetY = (avail.y - imageSize.y) * 0.5f;
            ImGui::SetCursorPos(ImVec2(cursor.x + (offsetX > 0.0f ? offsetX : 0.0f),
                                       cursor.y + (offsetY > 0.0f ? offsetY : 0.0f)));

            ImGui::Image(static_cast<ImTextureID>(rendererTexture_),
                         imageSize,
                         ImVec2(0.0f, 1.0f),
                         ImVec2(1.0f, 0.0f));
        } else {
            ImGui::TextUnformatted("Renderer not ready.");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void EditorUI::DrawRendererTab() {
    if (!ImGui::BeginTabItem("Renderer")) {
        return;
    }

    if (rendererTexture_ != 0 && rendererTextureWidth_ > 0 && rendererTextureHeight_ > 0) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float srcAspect = static_cast<float>(rendererTextureWidth_) / static_cast<float>(rendererTextureHeight_);
        ImVec2 imageSize = avail;
        if (imageSize.x > 0.0f && imageSize.y > 0.0f) {
            const float dstAspect = imageSize.x / imageSize.y;
            if (dstAspect > srcAspect) {
                imageSize.x = imageSize.y * srcAspect;
            } else {
                imageSize.y = imageSize.x / srcAspect;
            }
        }

        // Center letterboxed image while preserving source aspect ratio.
        const ImVec2 cursor = ImGui::GetCursorPos();
        const float offsetX = (avail.x - imageSize.x) * 0.5f;
        const float offsetY = (avail.y - imageSize.y) * 0.5f;
        ImGui::SetCursorPos(ImVec2(cursor.x + (offsetX > 0.0f ? offsetX : 0.0f),
                                   cursor.y + (offsetY > 0.0f ? offsetY : 0.0f)));

        ImGui::Image(static_cast<ImTextureID>(rendererTexture_),
                     imageSize,
                     ImVec2(0.0f, 1.0f),
                     ImVec2(1.0f, 0.0f));
    } else {
        ImGui::TextUnformatted("Renderer not ready.");
    }

    ImGui::EndTabItem();
}

void EditorUI::DrawConsoleTab(const std::string& currentWorkspace) {
    if (!ImGui::BeginTabItem("Console")) {
        return;
    }

    const ImVec4 consoleCommandColor = IsLightTheme() ? ImVec4(0.12f, 0.42f, 0.78f, 1.0f)
                                                       : ImVec4(0.6f, 0.9f, 1.0f, 1.0f);
    const float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("ConsoleOutput", ImVec2(0.0f, -footerHeight), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::scoped_lock lock(consoleMutex);
        if (auto it = workspaceConsoleLines_.find(currentWorkspace); it != workspaceConsoleLines_.end()) {
            for (const std::string& line : it->second) {
                if (!line.empty() && line[0] == '>') {
                    ImGui::TextColored(consoleCommandColor, "%s", line.c_str());
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
        }
        if (consoleScrollToBottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
            consoleScrollToBottom = false;
        }
    }
    ImGui::EndChild();
    ImGui::Separator();

    bool reclaimFocus = false;
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::InputText("##ConsoleInput",
                         commandBuffer.data(),
                         commandBuffer.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string command(commandBuffer.data());
        if (!command.empty()) {
            AddConsoleOutput("> " + command);
            AddConsoleOutput("[JIT not yet connected]");
        }
        commandBuffer[0] = '\0';
        reclaimFocus = true;
    }
    ImGui::PopItemWidth();
    ImGui::SetItemDefaultFocus();
    if (reclaimFocus) {
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::EndTabItem();
}

void EditorUI::DrawLogsTab(const std::string& currentWorkspace) {
    if (!ImGui::BeginTabItem("Logs")) {
        return;
    }

    const ImVec4 logErrorColor = IsLightTheme() ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f)
                                                : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    const ImVec4 logSuccessColor = IsLightTheme() ? ImVec4(0.16f, 0.56f, 0.24f, 1.0f)
                                                  : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);

    ImGui::BeginChild("LogsRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::scoped_lock lock(logMutex);
        if (auto it = workspaceLogLines_.find(currentWorkspace); it != workspaceLogLines_.end()) {
            for (const std::string& line : it->second) {
                if (line.find("Error") != std::string::npos || line.find("Failed") != std::string::npos) {
                    ImGui::TextColored(logErrorColor, "%s", line.c_str());
                } else if (line.find("[AutoSave]") != std::string::npos) {
                    ImGui::TextColored(logSuccessColor, "%s", line.c_str());
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
        }
        if (logScrollToBottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
            logScrollToBottom = false;
        }
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
}

void EditorUI::DrawConsolePane() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
    const bool lightTheme = IsLightTheme();
    const ImVec4 utilityPaneBg = lightTheme ? kLightUtilityPaneBgColor : kUtilityPaneBgColor;
    const ImVec4 utilityPaneChildBg = lightTheme ? kLightUtilityPaneChildBgColor : kUtilityPaneChildBgColor;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, utilityPaneBg);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, utilityPaneChildBg);
    ImGui::Begin("Utility View", nullptr, flags);

    const std::string currentWorkspace = ResolveCurrentWorkspaceName();
    if (ImGui::BeginTabBar("UtilityTabs", ImGuiTabBarFlags_None)) {
        DrawRendererTab();
        DrawConsoleTab(currentWorkspace);
        DrawLogsTab(currentWorkspace);
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
}

void EditorUI::Render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO &io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow *backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);
    }
}

void EditorUI::Shutdown() {
    if (shutdown_ || !initialized_) {
        return;
    }
    shutdown_ = true;
    initialized_ = false;

    NFD_Quit();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    window = nullptr;
}
