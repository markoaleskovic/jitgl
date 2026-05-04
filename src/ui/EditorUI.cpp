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

namespace {
    constexpr double AUTOSAVE_DEBOUNCE_SECONDS = 0.05;
    constexpr const char* GLSL_VERSION = "#version 330 core";
    constexpr std::size_t MAX_CONSOLE_LINES = 2000;
    constexpr std::size_t MAX_LOG_LINES = 2000;
    constexpr const char* FONT_PATH = "assets/JetBrainsMono-Regular.ttf";
    constexpr float BASE_FONT_SIZE = 16.0f;
    constexpr const char* FALLBACK_WORKSPACE_NAME = "default";

    const ImVec4 kEditorPaneBgColor = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    const ImVec4 kPanelBgColor = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    const ImVec4 kUtilityPaneBgColor = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    const ImVec4 kUtilityPaneChildBgColor = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

    bool IsKeyDown(GLFWwindow* window, int key) {
        return window != nullptr && glfwGetKey(window, key) == GLFW_PRESS;
    }

    std::string BuildWorkspaceDisplayLabel(const std::string& workspaceName, std::size_t index) {
        return std::to_string(index + 1) + ": " + workspaceName;
    }
}

// style start

void EditorUI::SetupDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
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

// style end

EditorUI::EditorUI() : window(nullptr), activeWorkspaceName_(FALLBACK_WORKSPACE_NAME) {}

EditorUI::~EditorUI() {
    Shutdown();
}

void EditorUI::ReloadFontAtlas(float dpiScale, bool recreateTexture) {
    ImGuiIO& io = ImGui::GetIO();

    if (recreateTexture) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
    }

    io.Fonts->Clear();

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = true;

    const float requestedSize = BASE_FONT_SIZE * dpiScale;
    const float pixelSize = std::round(requestedSize);

    ImFont* font = io.Fonts->AddFontFromFileTTF(FONT_PATH, pixelSize, &fontConfig);
    if (!font) {
        font = io.Fonts->AddFontDefault();
    }

    io.FontDefault = font;
    io.FontGlobalScale = 1.0f;
    io.Fonts->Build();

    if (recreateTexture) {
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
}

void EditorUI::Init(GLFWwindow *win) {
    window = win;

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

    ReloadFontAtlas(currentDpiScale_, false);

    SetupDarkTheme();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);

    initialized_ = true;
    shutdown_ = false;
}

void EditorUI::ApplyPendingDpiScale() {
    if (std::abs(pendingDpiScale_ - currentDpiScale_) < 0.001f) {
        return;
    }

    const float ratio = pendingDpiScale_ / currentDpiScale_;
    ImGui::GetStyle().ScaleAllSizes(ratio);
    ReloadFontAtlas(pendingDpiScale_, true);
    currentDpiScale_ = pendingDpiScale_;
}

void EditorUI::NewFrame() {
    ApplyPendingDpiScale();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EditorUI::Draw() {
    HandleGlobalShortcuts();
    DrawMenuBar();
    SetupDockspace();
    DrawTextEditorPane();
    DrawConsolePane();
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
    auto palette = TextEditor::GetDarkPalette();
    palette[(int)TextEditor::PaletteIndex::Background] = 0xff1e1e1e; // Matches your bgColor (#1e1e1e) in ImVec4(0.12, 0.12, 0.12)
    doc.editor.SetPalette(palette);
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

void EditorUI::SetWorkspaces(const std::vector<std::string>& workspaceNames, const std::string& activeWorkspace) {
    workspaceNames_ = workspaceNames;

    openDocuments.erase(std::remove_if(openDocuments.begin(),
                                       openDocuments.end(),
                                       [&](const Document& doc) {
                                           return std::find(workspaceNames_.begin(),
                                                            workspaceNames_.end(),
                                                            doc.workspaceName) == workspaceNames_.end();
                                       }),
                        openDocuments.end());

    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        for (auto it = workspaceConsoleLines_.begin(); it != workspaceConsoleLines_.end();) {
            if (std::find(workspaceNames_.begin(), workspaceNames_.end(), it->first) == workspaceNames_.end()) {
                it = workspaceConsoleLines_.erase(it);
            } else {
                ++it;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(logMutex);
        for (auto it = workspaceLogLines_.begin(); it != workspaceLogLines_.end();) {
            if (std::find(workspaceNames_.begin(), workspaceNames_.end(), it->first) == workspaceNames_.end()) {
                it = workspaceLogLines_.erase(it);
            } else {
                ++it;
            }
        }
    }

    SetActiveWorkspace(activeWorkspace);
}

void EditorUI::SetActiveWorkspace(const std::string& workspaceName) {
    if (workspaceName.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
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
        std::lock_guard<std::mutex> lock(consoleMutex);
        workspaceConsoleLines_[workspaceName] = std::move(consoleHistory);
    }
    {
        std::lock_guard<std::mutex> lock(logMutex);
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

void EditorUI::HandleGlobalShortcuts() {
    const bool ctrlHeld = IsKeyDown(window, GLFW_KEY_LEFT_CONTROL) || IsKeyDown(window, GLFW_KEY_RIGHT_CONTROL);
    const bool superHeld = IsKeyDown(window, GLFW_KEY_LEFT_SUPER) || IsKeyDown(window, GLFW_KEY_RIGHT_SUPER);
    const bool canUseCtrlShortcuts = ctrlHeld && !superHeld;
    const bool tabHeld = IsKeyDown(window, GLFW_KEY_TAB);
    const bool plusHeld = IsKeyDown(window, GLFW_KEY_EQUAL) || IsKeyDown(window, GLFW_KEY_KP_ADD);
    const bool minusHeld = IsKeyDown(window, GLFW_KEY_MINUS) || IsKeyDown(window, GLFW_KEY_KP_SUBTRACT);
    const bool workspaceCycleHeld = IsKeyDown(window, GLFW_KEY_GRAVE_ACCENT);

    const bool ctrlWorkspaceCycleHeld = canUseCtrlShortcuts && workspaceCycleHeld;
    if (ctrlWorkspaceCycleHeld && !ctrlWorkspaceCycleChordHeld_) {
        CycleWorkspace(1);
    }
    ctrlWorkspaceCycleChordHeld_ = ctrlWorkspaceCycleHeld;

    auto numberShortcutHeld = [this, canUseCtrlShortcuts](std::size_t index) {
        if (!canUseCtrlShortcuts) {
            return false;
        }
        switch (index) {
            case 0: return IsKeyDown(window, GLFW_KEY_1) || IsKeyDown(window, GLFW_KEY_KP_1);
            case 1: return IsKeyDown(window, GLFW_KEY_2) || IsKeyDown(window, GLFW_KEY_KP_2);
            case 2: return IsKeyDown(window, GLFW_KEY_3) || IsKeyDown(window, GLFW_KEY_KP_3);
            case 3: return IsKeyDown(window, GLFW_KEY_4) || IsKeyDown(window, GLFW_KEY_KP_4);
            case 4: return IsKeyDown(window, GLFW_KEY_5) || IsKeyDown(window, GLFW_KEY_KP_5);
            case 5: return IsKeyDown(window, GLFW_KEY_6) || IsKeyDown(window, GLFW_KEY_KP_6);
            case 6: return IsKeyDown(window, GLFW_KEY_7) || IsKeyDown(window, GLFW_KEY_KP_7);
            case 7: return IsKeyDown(window, GLFW_KEY_8) || IsKeyDown(window, GLFW_KEY_KP_8);
            case 8: return IsKeyDown(window, GLFW_KEY_9) || IsKeyDown(window, GLFW_KEY_KP_9);
            case 9: return IsKeyDown(window, GLFW_KEY_0) || IsKeyDown(window, GLFW_KEY_KP_0);
            default: return false;
        }
    };
    for (std::size_t i = 0; i < ctrlWorkspaceIndexChordHeld_.size(); ++i) {
        const bool held = numberShortcutHeld(i);
        if (held && !ctrlWorkspaceIndexChordHeld_[i]) {
            ActivateWorkspaceByIndex(i);
        }
        ctrlWorkspaceIndexChordHeld_[i] = held;
    }

    const bool ctrlTabHeld = canUseCtrlShortcuts && tabHeld;
    if (ctrlTabHeld && !ctrlTabChordHeld_) {
        ToggleActiveWorkspaceDocument();
    }
    ctrlTabChordHeld_ = ctrlTabHeld;

    const bool ctrlPlusHeld = canUseCtrlShortcuts && plusHeld;
    if (ctrlPlusHeld && !ctrlPlusChordHeld_) {
        SetDpiScale(currentDpiScale_ + 0.1f);
    }
    ctrlPlusChordHeld_ = ctrlPlusHeld;

    const bool ctrlMinusHeld = canUseCtrlShortcuts && minusHeld;
    if (ctrlMinusHeld && !ctrlMinusChordHeld_) {
        SetDpiScale(currentDpiScale_ - 0.1f);
    }
    ctrlMinusChordHeld_ = ctrlMinusHeld;
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
        const std::size_t currentPosition = static_cast<std::size_t>(
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

    const std::size_t nextWorkspaceIndex = static_cast<std::size_t>(nextIndex);
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

void EditorUI::DrawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        const std::vector<std::string> workspaceNamesSnapshot = workspaceNames_;
        const bool canDeleteAnyWorkspace = workspaceNamesSnapshot.size() > 1;
        std::string pendingWorkspaceSwitch;
        std::string pendingWorkspaceDelete;

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Workspace...")) {
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
                        pendingWorkspaceDelete = workspaceName;
                    }
                }
                if (!canDeleteAnyWorkspace) {
                    ImGui::EndDisabled();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Workspace")) {
            for (std::size_t i = 0; i < workspaceNamesSnapshot.size(); ++i) {
                const std::string& workspaceName = workspaceNamesSnapshot[i];
                const std::string workspaceLabel = BuildWorkspaceDisplayLabel(workspaceName, i);
                const bool selected = (workspaceName == activeWorkspaceName_);
                if (ImGui::MenuItem(workspaceLabel.c_str(), nullptr, selected)) {
                    pendingWorkspaceSwitch = workspaceName;
                }
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("View")) { 
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
            ImGui::EndMenu(); 
        }
        
        if (ImGui::BeginMenu("Help")) { ImGui::EndMenu(); }

        if (openCreateWorkspacePopup_) {
            openCreateWorkspacePopup_ = false;
            newWorkspaceNameBuffer_[0] = '\0';
            ImGui::OpenPopup("Create Workspace");
        }

        if (ImGui::BeginPopupModal("Create Workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("Name", newWorkspaceNameBuffer_, IM_ARRAYSIZE(newWorkspaceNameBuffer_));
            const bool canCreate = newWorkspaceNameBuffer_[0] != '\0';

            if (!canCreate) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Create")) {
                if (onCreateWorkspace_) {
                    onCreateWorkspace_(newWorkspaceNameBuffer_);
                }
                newWorkspaceNameBuffer_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            if (!canCreate) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                newWorkspaceNameBuffer_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        std::string currentWorkspace;
        {
            std::lock_guard<std::mutex> lock(workspaceMutex_);
            currentWorkspace = activeWorkspaceName_;
        }
        if (!currentWorkspace.empty()) {
            std::string workspaceLabel = "Workspace: " + currentWorkspace;
            auto activeWorkspaceIt = std::find(workspaceNamesSnapshot.begin(),
                                               workspaceNamesSnapshot.end(),
                                               currentWorkspace);
            if (activeWorkspaceIt != workspaceNamesSnapshot.end()) {
                const std::size_t activeWorkspaceIndex = static_cast<std::size_t>(
                    std::distance(workspaceNamesSnapshot.begin(), activeWorkspaceIt));
                workspaceLabel = "Workspace " + std::to_string(activeWorkspaceIndex + 1) + ": " + currentWorkspace;
            }
            const float centeredX = (ImGui::GetWindowWidth() - ImGui::CalcTextSize(workspaceLabel.c_str()).x) * 0.5f;
            if (centeredX > 0.0f) {
                ImGui::SetCursorPosX(centeredX);
            }
            ImGui::TextColored(ImVec4(0.82f, 0.82f, 0.82f, 1.0f), "%s", workspaceLabel.c_str());
        }

        if (!pendingWorkspaceDelete.empty() && onDeleteWorkspace_) {
            onDeleteWorkspace_(pendingWorkspaceDelete);
        } else if (!pendingWorkspaceSwitch.empty()) {
            SetActiveWorkspace(pendingWorkspaceSwitch);
            if (onWorkspaceSwitched_) {
                onWorkspaceSwitched_(pendingWorkspaceSwitch);
            }
        }

        // Compile Status Indicator
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 170.0f);
        if (isStalled_) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f), "STALLED?");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("JIT compilation is taking longer than expected.\nCheck Logs for details.");
            }
        } else if (isCompiling_) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "COMPILING...");
        } else if (hasCompileError_) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "ERROR");
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "READY");
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorUI::DrawTextEditorPane() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("Code Editor", nullptr, flags);

    constexpr float workspaceSidebarWidth = 165.0f;
    ImGui::BeginChild("WorkspaceSidebar", ImVec2(workspaceSidebarWidth, 0.0f), true);
    ImGui::TextUnformatted("Workspaces");
    ImGui::Separator();

    const std::vector<std::string> workspaceNamesSnapshot = workspaceNames_;
    const bool canDeleteAnyWorkspace = workspaceNamesSnapshot.size() > 1;
    std::string pendingWorkspaceSwitch;
    std::string pendingWorkspaceDelete;
    for (std::size_t i = 0; i < workspaceNamesSnapshot.size(); ++i) {
        const std::string& workspaceName = workspaceNamesSnapshot[i];
        const std::string workspaceLabel = BuildWorkspaceDisplayLabel(workspaceName, i);
        ImGui::PushID(workspaceName.c_str());
        const bool selected = (workspaceName == activeWorkspaceName_);
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const float deleteButtonWidth = canDeleteAnyWorkspace ? (ImGui::GetFrameHeight() - 2.0f) : 0.0f;
        const float selectableWidth = canDeleteAnyWorkspace ? std::max(1.0f, rowWidth - deleteButtonWidth - 6.0f)
                                                            : rowWidth;
        if (ImGui::Selectable(workspaceLabel.c_str(), selected, 0, ImVec2(selectableWidth, 0.0f))) {
            pendingWorkspaceSwitch = workspaceName;
        }

        if (ImGui::BeginPopupContextItem("WorkspaceSidebarContext")) {
            if (canDeleteAnyWorkspace) {
                if (ImGui::MenuItem("Delete Workspace")) {
                    pendingWorkspaceDelete = workspaceName;
                }
            } else {
                ImGui::TextDisabled("Cannot delete last workspace");
            }
            ImGui::EndPopup();
        }

        if (canDeleteAnyWorkspace) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                pendingWorkspaceDelete = workspaceName;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Delete workspace");
            }
        }

        ImGui::PopID();
    }

    if (!pendingWorkspaceDelete.empty() && onDeleteWorkspace_) {
        onDeleteWorkspace_(pendingWorkspaceDelete);
    } else if (!pendingWorkspaceSwitch.empty()) {
        SetActiveWorkspace(pendingWorkspaceSwitch);
        if (onWorkspaceSwitched_) {
            onWorkspaceSwitched_(pendingWorkspaceSwitch);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("WorkspaceEditorArea", ImVec2(0.0f, 0.0f), false);
    if (ImGui::BeginTabBar("EditorTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        double currentTime = ImGui::GetTime();
        bool drewAnyDocument = false;
        bool pendingSelectionVisible = false;
        bool pendingSelectionConsumed = false;

        for (auto &doc: openDocuments) {
            if (!doc.isOpen) continue;
            if (doc.workspaceName != activeWorkspaceName_) continue;
            drewAnyDocument = true;

            ImGuiTabItemFlags tabFlags = doc.isDirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
            if (!pendingDocumentSelectionPath_.empty() &&
                doc.filepath == pendingDocumentSelectionPath_) {
                pendingSelectionVisible = true;
                tabFlags |= ImGuiTabItemFlags_SetSelected;
            }

            if (ImGui::BeginTabItem(doc.filename.c_str(), &doc.isOpen, tabFlags)) {
                if (!pendingDocumentSelectionPath_.empty() &&
                    doc.filepath == pendingDocumentSelectionPath_) {
                    pendingSelectionConsumed = true;
                }
                if (activeDocumentPath_ != doc.filepath) {
                    activeDocumentPath_ = doc.filepath;
                    if (onActiveDocumentChanged_) {
                        onActiveDocumentChanged_(doc.filepath, doc.lastKnownText);
                    }
                }

                doc.editor.Render(doc.filename.c_str());
                std::string currentText = doc.editor.GetText();

                if (currentText != doc.lastKnownText) {
                    doc.isDirty = true;
                    doc.lastModifiedTime = currentTime;
                    doc.lastKnownText = std::move(currentText); // Move semantics avoid a string copy here
                    if (onDocumentChanged_) {
                        onDocumentChanged_(doc.filepath, doc.lastKnownText);
                    }
                }
                ImGui::EndTabItem();
            }
        }

        if (!pendingDocumentSelectionPath_.empty() &&
            (pendingSelectionConsumed || !pendingSelectionVisible)) {
            pendingDocumentSelectionPath_.clear();
        }

        for (auto& doc : openDocuments) {
            if (!doc.isDirty || (currentTime - doc.lastModifiedTime) < AUTOSAVE_DEBOUNCE_SECONDS) {
                continue;
            }

            bool saved = false;
            if (onSaveDocument_) {
                // Use the already-cached text instead of querying the editor again
                saved = onSaveDocument_(doc.filepath, doc.lastKnownText);
            }

            if (saved) {
                doc.isDirty = false;
                AddLogOutput("[AutoSave] " + doc.filename + " committed to disk.");
            } else {
                AddLogOutput("[AutoSave Error] Failed to write " + doc.filepath);
            }
        }

        if (!drewAnyDocument) {
            ImGui::TextUnformatted("No files in this workspace.");
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}
void EditorUI::AddConsoleOutput(const std::string &text) {
    std::string workspaceName;
    {
        std::lock_guard<std::mutex> workspaceLock(workspaceMutex_);
        workspaceName = activeWorkspaceName_;
    }
    if (workspaceName.empty()) {
        workspaceName = FALLBACK_WORKSPACE_NAME;
    }

    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        auto& lines = workspaceConsoleLines_[workspaceName];
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
        std::lock_guard<std::mutex> workspaceLock(workspaceMutex_);
        workspaceName = activeWorkspaceName_;
    }
    if (workspaceName.empty()) {
        workspaceName = FALLBACK_WORKSPACE_NAME;
    }

    {
        std::lock_guard<std::mutex> lock(logMutex);
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

void EditorUI::DrawConsolePane() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kUtilityPaneBgColor);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kUtilityPaneChildBgColor);
    ImGui::Begin("Utility View", nullptr, flags);

    std::string currentWorkspace;
    {
        std::lock_guard<std::mutex> workspaceLock(workspaceMutex_);
        currentWorkspace = activeWorkspaceName_;
    }
    if (currentWorkspace.empty()) {
        currentWorkspace = FALLBACK_WORKSPACE_NAME;
    }

    if (ImGui::BeginTabBar("UtilityTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Renderer")) {
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
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Console")) {
            const float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
            ImGui::BeginChild("ConsoleOutput", ImVec2(0.0f, -footerHeight), false, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> lock(consoleMutex);
                auto it = workspaceConsoleLines_.find(currentWorkspace);
                if (it != workspaceConsoleLines_.end()) {
                    for (const std::string& line : it->second) {
                        if (!line.empty() && line[0] == '>') {
                            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", line.c_str());
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
            if (ImGui::InputText("##ConsoleInput", commandBuffer, IM_ARRAYSIZE(commandBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
                std::string command(commandBuffer);
                if (!command.empty()) {
                    AddConsoleOutput("> " + command);
                    AddConsoleOutput("[JIT not yet connected]");
                }
                commandBuffer[0] = '\0';
                reclaimFocus = true;
            }
            ImGui::PopItemWidth();
            ImGui::SetItemDefaultFocus();
            if (reclaimFocus) ImGui::SetKeyboardFocusHere(-1);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Logs")) {
            ImGui::BeginChild("LogsRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> lock(logMutex);
                auto it = workspaceLogLines_.find(currentWorkspace);
                if (it != workspaceLogLines_.end()) {
                    for (const std::string& line : it->second) {
                        if (line.find("Error") != std::string::npos || line.find("Failed") != std::string::npos) {
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", line.c_str());
                        } else if (line.find("[AutoSave]") != std::string::npos) {
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", line.c_str());
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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    window = nullptr;
}
