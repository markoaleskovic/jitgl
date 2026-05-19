#include "ui/EditorUI.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <string>
#include <imgui_internal.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <ranges>
#include <sstream>
#include <type_traits>
#include <nfd.hpp>

namespace {
    constexpr double AUTOSAVE_DEBOUNCE_SECONDS = 0.05;
    constexpr const char* GLSL_VERSION = "#version 330 core";
    constexpr std::size_t MAX_CONSOLE_LINES = 2000;
    constexpr std::size_t MAX_LOG_LINES = 2000;
    constexpr std::size_t MAX_PENDING_SHARE_OFFERS = 64;
    constexpr const char* FONT_PATH = "assets/JetBrainsMono-Regular.ttf";
    constexpr float BASE_FONT_SIZE = 16.0f;
    constexpr const char* FALLBACK_WORKSPACE_NAME = "default";
    constexpr double DPI_APPLY_MIN_INTERVAL_SECONDS = 0.08;
    constexpr ImGuiWindowFlags kCenteredModalFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;

    const ImVec4 kEditorPaneBgColor = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    const ImVec4 kPanelBgColor = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    const ImVec4 kUtilityPaneBgColor = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    const ImVec4 kUtilityPaneChildBgColor = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    const ImVec4 kLightEditorPaneBgColor = ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    const ImVec4 kLightPanelBgColor = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    const ImVec4 kLightUtilityPaneBgColor = ImVec4(0.89f, 0.89f, 0.89f, 1.00f);
    const ImVec4 kLightUtilityPaneChildBgColor = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);

    class ManagedCenteredModal {
    public:
        ManagedCenteredModal(const char* popupName,
                             bool* openRequested,
                             bool* openedThisSession)
            : popupName_(popupName),
              openRequested_(openRequested),
              openedThisSession_(openedThisSession) {
        }

        template <typename OnOpenFn = std::nullptr_t>
        bool Begin(const ImVec2& openingSize, const ImVec2& minSize, OnOpenFn&& onOpen = nullptr) const {
            if (*openRequested_ && !*openedThisSession_) {
                if constexpr (!std::is_same_v<std::remove_cvref_t<OnOpenFn>, std::nullptr_t>) {
                    onOpen();
                }
                ImGui::OpenPopup(popupName_);
                *openedThisSession_ = true;
            }

            if (*openedThisSession_ && !ImGui::IsPopupOpen(popupName_)) {
                *openRequested_ = false;
                *openedThisSession_ = false;
                return false;
            }

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(openingSize, ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(minSize,
                                                ImVec2(viewport->WorkSize.x * 0.96f, viewport->WorkSize.y * 0.96f));
            return ImGui::BeginPopupModal(popupName_, nullptr, kCenteredModalFlags);
        }

        void CloseCurrent() const {
            *openRequested_ = false;
            *openedThisSession_ = false;
            ImGui::CloseCurrentPopup();
        }

    private:
        const char* popupName_;
        bool* openRequested_;
        bool* openedThisSession_;
    };

    class TextureLayout {
    public:
        struct PlacedImage {
            bool drawn = false;
            ImVec2 screenMin{};
            ImVec2 screenMax{};
        };

        static bool DrawCentered(unsigned int texture,
                                 int width,
                                 int height,
                                 const char* fallbackText,
                                 PlacedImage* outPlacedImage = nullptr) {
            if (outPlacedImage != nullptr) {
                *outPlacedImage = {};
            }
            if (texture == 0 || width <= 0 || height <= 0) {
                ImGui::TextUnformatted(fallbackText);
                return false;
            }

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 imageSize = FitInside(avail, width, height);
            const ImVec2 cursor = ImGui::GetCursorPos();
            const float offsetX = (avail.x - imageSize.x) * 0.5f;
            const float offsetY = (avail.y - imageSize.y) * 0.5f;
            ImGui::SetCursorPos(ImVec2(cursor.x + (offsetX > 0.0f ? offsetX : 0.0f),
                                       cursor.y + (offsetY > 0.0f ? offsetY : 0.0f)));
            const ImVec2 imageScreenMin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(texture), imageSize, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            if (outPlacedImage != nullptr) {
                outPlacedImage->drawn = true;
                outPlacedImage->screenMin = imageScreenMin;
                outPlacedImage->screenMax = ImVec2(imageScreenMin.x + imageSize.x,
                                                   imageScreenMin.y + imageSize.y);
            }
            return true;
        }

    private:
        static ImVec2 FitInside(const ImVec2& available, int width, int height) {
            ImVec2 size = available;
            if (size.x <= 0.0f || size.y <= 0.0f || width <= 0 || height <= 0) {
                return size;
            }

            const float sourceAspect = static_cast<float>(width) / static_cast<float>(height);
            const float targetAspect = size.x / size.y;
            if (targetAspect > sourceAspect) {
                size.x = size.y * sourceAspect;
            } else {
                size.y = size.x / sourceAspect;
            }
            return size;
        }
    };

    void DrawNetworkStatusRow(const char* label, bool value, const ImVec4& okColor, const ImVec4& badColor) {
        ImGui::Text("%s", label);
        ImGui::SameLine();
        ImGui::TextColored(value ? okColor : badColor, "%s", value ? "OK" : "NO");
    }

    void DrawNetworkAgeRow(const char* label, double ageSeconds) {
        if (ageSeconds >= 0.0) {
            ImGui::Text("%s %.2f sec ago", label, ageSeconds);
        } else {
            ImGui::Text("%s never", label);
        }
    }

    bool IsKeyDown(GLFWwindow* window, int key) {
        return window != nullptr && glfwGetKey(window, key) == GLFW_PRESS;
    }

    std::string BuildWorkspaceDisplayLabel(const std::string& workspaceName, std::size_t index) {
        return std::to_string(index + 1) + ": " + workspaceName;
    }

    std::string TrimString(std::string value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    double AgeSeconds(double nowSeconds, double eventSeconds) {
        if (eventSeconds <= 0.0 || nowSeconds <= 0.0 || nowSeconds < eventSeconds) {
            return -1.0;
        }
        return nowSeconds - eventSeconds;
    }

    const char* YesNo(bool value) {
        return value ? "Yes" : "No";
    }

    const char* PipelineGlobalUniformTypeLabel(const EditorUI::PipelineGlobalUniformType type) {
        switch (type) {
            case EditorUI::PipelineGlobalUniformType::Float: return "float";
            case EditorUI::PipelineGlobalUniformType::Int: return "int";
            case EditorUI::PipelineGlobalUniformType::Bool: return "bool";
            case EditorUI::PipelineGlobalUniformType::Vec4: return "vec4";
        }
        return "float";
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
    showWelcomeOnStartup_ = appPreferences_.GetBool("welcome.show_on_startup", true);
}

void EditorUI::SaveWelcomePreference() {
    appPreferences_.SetBool("welcome.show_on_startup", showWelcomeOnStartup_);
    (void)appPreferences_.Save();
}

void EditorUI::LoadShowcasePreference() {
    loadShowcaseWorkspaceOnStartup_ = appPreferences_.GetBool("showcase.load_on_startup", true);
}

void EditorUI::SaveShowcasePreference() {
    appPreferences_.SetBool("showcase.load_on_startup", loadShowcaseWorkspaceOnStartup_);
    (void)appPreferences_.Save();
}

void EditorUI::LoadNetworkPreference() {
    networkEnabled_ = appPreferences_.GetBool("network.enabled", true);
}

void EditorUI::SaveNetworkPreference() {
    appPreferences_.SetBool("network.enabled", networkEnabled_);
    (void)appPreferences_.Save();
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

    (void)appPreferences_.Reload();
    LoadWelcomePreference();
    LoadShowcasePreference();
    LoadNetworkPreference();
    LoadAllSettingsFromPrefs();
    openWelcomePopupRequested_ = showWelcomeOnStartup_;
    welcomePopupOpenedThisSession_ = false;
    doNotShowWelcomeAgain_ = false;
    openShowcaseGuidePopupRequested_ = loadShowcaseWorkspaceOnStartup_;
    showcaseGuidePopupOpenedThisSession_ = false;
    disableShowcaseStartupFromGuide_ = !loadShowcaseWorkspaceOnStartup_;

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
    rendererTabActive_ = false;
    // Wipe the cached renderer rect each frame. DrawRendererTab /
    // DrawRendererFullscreen repopulate it when they actually draw the
    // image; if neither runs (renderer tab not selected, etc.) the host
    // sees visible=false and stops forwarding mouse position.
    rendererViewportState_ = RendererViewportState{};

    if (rendererFullscreen_) {
        DrawRendererFullscreen();
    } else {
        DrawMenuBar();
        SetupDockspace();
        DrawTextEditorPane();
        DrawConsolePane();
    }

    DrawUniformsTab();
    DrawIncomingWorkspaceSharePopup();
    DrawNetworkDiagnosticsWindow();
    DrawWelcomePopup();
    DrawShowcaseGuidePopup();
    DrawRuntimeGuidePopup();
    DrawSettingsWindow();
    DrawFpsOverlay();
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

void EditorUI::SetShareWorkspaceCallback(std::function<void(const std::vector<std::string>&, bool)> cb) {
    onShareWorkspace_ = std::move(cb);
}

void EditorUI::SetWorkspaceShareDecisionCallback(std::function<void(const std::string&, bool)> cb) {
    onWorkspaceShareDecision_ = std::move(cb);
}

void EditorUI::SetRequestFirewallAccessCallback(std::function<void()> cb) {
    onRequestFirewallAccess_ = std::move(cb);
}

void EditorUI::SetHardResetRuntimeCallback(std::function<void()> cb) {
    onHardResetRuntime_ = std::move(cb);
}

void EditorUI::SetNetworkEnabledChangedCallback(std::function<void(bool)> cb) {
    onNetworkEnabledChanged_ = std::move(cb);
}

bool EditorUI::IsNetworkEnabled() const {
    return networkEnabled_;
}

void EditorUI::SetNetworkPeers(std::vector<NetworkPeer> peers) {
    networkPeers_ = std::move(peers);

    std::erase_if(selectedNetworkPeers_,
                  [this](const auto& entry) {
                      return std::ranges::none_of(networkPeers_,
                                                  [&](const NetworkPeer& peer) { return peer.id == entry.first; });
                  });
}

void EditorUI::SetNetworkDiagnostics(NetworkDiagnostics diagnostics) {
    networkDiagnostics_ = std::move(diagnostics);
}

void EditorUI::QueueIncomingWorkspaceShareOffer(IncomingWorkspaceShareOffer offer) {
    if (offer.offerId.empty()) {
        return;
    }
    const bool alreadyQueued = std::ranges::any_of(pendingWorkspaceShareOffers_,
                                                    [&](const IncomingWorkspaceShareOffer& existing) {
                                                        return existing.offerId == offer.offerId;
                                                    });
    if (!alreadyQueued) {
        if (pendingWorkspaceShareOffers_.size() >= MAX_PENDING_SHARE_OFFERS) {
            pendingWorkspaceShareOffers_.erase(pendingWorkspaceShareOffers_.begin());
        }
        pendingWorkspaceShareOffers_.push_back(std::move(offer));
    }
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

EditorUI::RendererOutputMode EditorUI::GetRendererOutputMode() const {
    return rendererOutputMode_;
}

void EditorUI::SetCompilationStatus(bool isCompiling, bool hasError, bool isStalled) {
    isCompiling_ = isCompiling;
    hasCompileError_ = hasError;
    isStalled_ = isStalled;
}

void EditorUI::SetUniformValues(std::vector<UniformValue> values) {
    uniformValues_ = std::move(values);
}

void EditorUI::SetUniformEditCallback(std::function<void(const UniformEditCommand&)> cb) {
    onUniformEdit_ = std::move(cb);
}

void EditorUI::SetUniformJsonSnapshotCallback(std::function<std::string()> cb) {
    onUniformJsonSnapshot_ = std::move(cb);
}

void EditorUI::SetPlaybackState(PlaybackState state) {
    playbackState_ = state;
}

void EditorUI::SetPlaybackCommandCallback(std::function<void(const PlaybackCommand&)> cb) {
    onPlaybackCommand_ = std::move(cb);
}

void EditorUI::SetLoadShowcaseWorkspaceCallback(std::function<void()> cb) {
    onLoadShowcaseWorkspace_ = std::move(cb);
}

void EditorUI::SetPipelinePasses(std::vector<PipelinePassView> passes) {
    pipelinePasses_ = std::move(passes);
}

void EditorUI::SetPipelineResources(std::vector<PipelineResourceView> resources) {
    pipelineResources_ = std::move(resources);
}

void EditorUI::SetPipelineConnections(std::vector<PipelineConnectionView> connections) {
    pipelineConnections_ = std::move(connections);
}

void EditorUI::SetPipelineDependencies(std::vector<PipelineDependencyView> dependencies) {
    pipelineDependencies_ = std::move(dependencies);
}

void EditorUI::SetPipelineGlobalUniforms(std::vector<PipelineGlobalUniformView> globals) {
    pipelineGlobalUniforms_ = std::move(globals);
}

void EditorUI::SetPipelineMoveCallback(std::function<void(const std::string&, int)> cb) {
    onPipelineMove_ = std::move(cb);
}

void EditorUI::SetPipelineEditCallback(std::function<void(const PipelineEditCommand&)> cb) {
    onPipelineEdit_ = std::move(cb);
}

void EditorUI::SetPipelineAddPassCallback(std::function<void(const std::string&)> cb) {
    onPipelineAddPass_ = std::move(cb);
}

void EditorUI::SetPipelineSaveChainCallback(std::function<bool(const std::string&)> cb) {
    onPipelineSaveChain_ = std::move(cb);
}

void EditorUI::SetPipelineResetCallback(std::function<void()> cb) {
    onPipelineReset_ = std::move(cb);
}

void EditorUI::SetPipelineOpenFileCallback(std::function<void(const std::string&, bool)> cb) {
    onPipelineOpenFile_ = std::move(cb);
}

void EditorUI::SetPipelineConnectionCallback(std::function<void(const PipelineConnectionCommand&)> cb) {
    onPipelineConnection_ = std::move(cb);
}

void EditorUI::SetPipelineGlobalUniformCallback(std::function<void(const PipelineGlobalUniformCommand&)> cb) {
    onPipelineGlobalUniform_ = std::move(cb);
}

void EditorUI::SetPipelineResourceExportCallback(std::function<bool(const std::string&, const std::string&)> cb) {
    onPipelineResourceExport_ = std::move(cb);
}

bool EditorUI::ShouldLoadShowcaseWorkspaceOnStartup() const {
    return loadShowcaseWorkspaceOnStartup_;
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
    const ManagedCenteredModal popup("Welcome to JITGL",
                                     &openWelcomePopupRequested_,
                                     &welcomePopupOpenedThisSession_);
    if (!popup.Begin(ImVec2(860.0f, 680.0f),
                     ImVec2(720.0f, 520.0f),
                     [this]() { doNotShowWelcomeAgain_ = !showWelcomeOnStartup_; })) {
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
        popup.CloseCurrent();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawRuntimeGuidePopup() {
    const ManagedCenteredModal popup("Runtime State Guide",
                                     &openRuntimeGuidePopupRequested_,
                                     &runtimeGuidePopupOpenedThisSession_);
    if (!popup.Begin(ImVec2(900.0f, 720.0f), ImVec2(760.0f, 560.0f))) {
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
        popup.CloseCurrent();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawShowcaseGuidePopup() {
    const ManagedCenteredModal popup("Showcase Workspace Guide",
                                     &openShowcaseGuidePopupRequested_,
                                     &showcaseGuidePopupOpenedThisSession_);
    if (!popup.Begin(ImVec2(940.0f, 740.0f),
                     ImVec2(760.0f, 560.0f),
                     [this]() { disableShowcaseStartupFromGuide_ = !loadShowcaseWorkspaceOnStartup_; })) {
        return;
    }

    const float footerReserve = ImGui::GetFrameHeightWithSpacing() * 2.8f;
    if (ImGui::BeginChild("ShowcaseGuideScrollableContent", ImVec2(0.0f, -footerReserve), false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        DrawMarkdown(showcaseGuideMarkdown_, IsLightTheme());
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Checkbox("Disable showcase workspace + guide on startup", &disableShowcaseStartupFromGuide_);

    if (ImGui::Button("Load Showcase Workspace Now")) {
        if (onLoadShowcaseWorkspace_) {
            onLoadShowcaseWorkspace_();
        }
    }

    const float closeButtonWidth = 100.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeButtonWidth);
    if (ImGui::Button("Continue", ImVec2(closeButtonWidth, 0.0f))) {
        loadShowcaseWorkspaceOnStartup_ = !disableShowcaseStartupFromGuide_;
        SaveShowcasePreference();
        popup.CloseCurrent();
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
    showcaseGuideMarkdown_ = loadFile("assets/showcase_guide.md");
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
    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_P),
                       &ctrlRenderModeChordHeld_,
                       [this]() {
                           rendererOutputMode_ = (rendererOutputMode_ == RendererOutputMode::ActiveWorkspace)
                                                     ? RendererOutputMode::PipelineChain
                                                     : RendererOutputMode::ActiveWorkspace;
                       });
    TriggerChordAction(canUseCtrlShortcuts && IsKeyDown(window, GLFW_KEY_COMMA),
                       &ctrlSettingsChordHeld_,
                       [this]() {
                           pendingSettings_ = activeSettings_;
                           showSettingsWindow_ = true;
                           settingsWindowJustOpened_ = true;
                       });
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
    if (ImGui::MenuItem("Settings...", "Ctrl+,")) {
        pendingSettings_ = activeSettings_;
        showSettingsWindow_ = true;
        settingsWindowJustOpened_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Hard Reset Runtime State")) {
        if (onHardResetRuntime_) {
            onHardResetRuntime_();
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Share Workspace...")) {
        openShareWorkspacePopup_ = true;
    }
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
    const bool workspaceRenderSelected = (rendererOutputMode_ == RendererOutputMode::ActiveWorkspace);
    if (ImGui::MenuItem("Render Source: Workspace", "Ctrl+P", workspaceRenderSelected)) {
        rendererOutputMode_ = workspaceRenderSelected ? RendererOutputMode::PipelineChain
                                                      : RendererOutputMode::ActiveWorkspace;
    }
    ImGui::Separator();
    (void)ImGui::MenuItem("Uniform Controls", nullptr, &showUniformControlsPanel_);
    (void)ImGui::MenuItem("Playback Controls", nullptr, &showPlaybackControlsPanel_);
    (void)ImGui::MenuItem("Network Diagnostics", nullptr, &showNetworkDiagnostics_);
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
    if (ImGui::MenuItem("Showcase Guide")) {
        openShowcaseGuidePopupRequested_ = true;
        showcaseGuidePopupOpenedThisSession_ = false;
    }
    bool startupEnabled = loadShowcaseWorkspaceOnStartup_;
    if (ImGui::MenuItem("Enable Showcase Startup", nullptr, &startupEnabled)) {
        loadShowcaseWorkspaceOnStartup_ = startupEnabled;
        SaveShowcasePreference();
        if (loadShowcaseWorkspaceOnStartup_) {
            openShowcaseGuidePopupRequested_ = true;
            showcaseGuidePopupOpenedThisSession_ = false;
            if (onLoadShowcaseWorkspace_) {
                onLoadShowcaseWorkspace_();
            }
        }
    }
    if (ImGui::MenuItem("Load Showcase Workspace Now")) {
        if (onLoadShowcaseWorkspace_) {
            onLoadShowcaseWorkspace_();
        }
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

void EditorUI::OpenShareWorkspacePopupIfRequested() {
    if (!openShareWorkspacePopup_) {
        return;
    }

    openShareWorkspacePopup_ = false;
    selectedNetworkPeers_.clear();
    ImGui::OpenPopup("Share Workspace");
}

void EditorUI::DrawShareWorkspacePopup() {
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const ImGuiWindowFlags sharePopupFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoMove;
    if (!ImGui::BeginPopupModal("Share Workspace", nullptr, sharePopupFlags)) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted("Choose computers on your LAN to share the active workspace.");
    ImGui::Separator();

    if (networkPeers_.empty()) {
        ImGui::TextDisabled("No peers discovered yet.");
    } else if (ImGui::BeginChild("ShareWorkspacePeerList", ImVec2(420.0f, 180.0f), true)) {
        for (const auto& peer : networkPeers_) {
            bool selected = selectedNetworkPeers_[peer.id];
            const std::string label = peer.displayName + " (" + peer.ipAddress + ")";
            if (ImGui::Checkbox(label.c_str(), &selected)) {
                selectedNetworkPeers_[peer.id] = selected;
            }
        }
        ImGui::EndChild();
    }

    std::vector<std::string> selectedPeerIds;
    selectedPeerIds.reserve(selectedNetworkPeers_.size());
    for (const auto& entry : selectedNetworkPeers_) {
        if (entry.second) {
            selectedPeerIds.push_back(entry.first);
        }
    }

    if (selectedPeerIds.empty()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Share Selected")) {
        if (onShareWorkspace_) {
            onShareWorkspace_(selectedPeerIds, false);
        }
        ImGui::CloseCurrentPopup();
    }
    if (selectedPeerIds.empty()) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Share To All")) {
        if (onShareWorkspace_) {
            onShareWorkspace_({}, true);
        }
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawIncomingWorkspaceSharePopup() {
    if (pendingWorkspaceShareOffers_.empty()) {
        workspaceSharePromptOpen_ = false;
        return;
    }

    if (!workspaceSharePromptOpen_) {
        ImGui::OpenPopup("Incoming Workspace Share");
        workspaceSharePromptOpen_ = true;
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const ImGuiWindowFlags incomingPopupFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                                ImGuiWindowFlags_NoResize |
                                                ImGuiWindowFlags_NoMove;
    if (!ImGui::BeginPopupModal("Incoming Workspace Share", nullptr, incomingPopupFlags)) {
        return;
    }

    const IncomingWorkspaceShareOffer offer = pendingWorkspaceShareOffers_.front();
    ImGui::Text("Load workspace '%s' from '%s'?",
                offer.workspaceName.c_str(),
                offer.senderName.c_str());
    ImGui::Separator();

    if (ImGui::Button("Load")) {
        if (onWorkspaceShareDecision_) {
            onWorkspaceShareDecision_(offer.offerId, true);
        }
        pendingWorkspaceShareOffers_.erase(pendingWorkspaceShareOffers_.begin());
        workspaceSharePromptOpen_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Decline")) {
        if (onWorkspaceShareDecision_) {
            onWorkspaceShareDecision_(offer.offerId, false);
        }
        pendingWorkspaceShareOffers_.erase(pendingWorkspaceShareOffers_.begin());
        workspaceSharePromptOpen_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorUI::DrawNetworkDiagnosticsWindow() {
    if (!showNetworkDiagnostics_) {
        return;
    }

    if (!ImGui::Begin("Network Diagnostics", &showNetworkDiagnostics_)) {
        ImGui::End();
        return;
    }

    bool networkEnabled = networkEnabled_;
    if (ImGui::Checkbox("Enable LAN Networking", &networkEnabled)) {
        networkEnabled_ = networkEnabled;
        SaveNetworkPreference();
        if (!networkEnabled_) {
            pendingWorkspaceShareOffers_.clear();
            workspaceSharePromptOpen_ = false;
            selectedNetworkPeers_.clear();
        }
        if (onNetworkEnabledChanged_) {
            onNetworkEnabledChanged_(networkEnabled_);
        }
    }

    ImGui::Separator();

    const ImVec4 okColor = IsLightTheme() ? ImVec4(0.16f, 0.56f, 0.24f, 1.0f)
                                          : ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    const ImVec4 badColor = IsLightTheme() ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f)
                                           : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

    DrawNetworkStatusRow("Service Running:", networkDiagnostics_.serviceRunning, okColor, badColor);
    DrawNetworkStatusRow("UDP Discovery Bound:", networkDiagnostics_.udpSocketBound, okColor, badColor);
    DrawNetworkStatusRow("TCP Transfer Bound:", networkDiagnostics_.tcpSocketBound, okColor, badColor);
    ImGui::Text("Multicast Join Attempted: %s", YesNo(networkDiagnostics_.multicastJoinAttempted));
    ImGui::Text("Multicast Join Succeeded: %s", YesNo(networkDiagnostics_.multicastJoinSucceeded));
    ImGui::Text("Discovery Port: %u", static_cast<unsigned int>(networkDiagnostics_.discoveryPort));
    ImGui::Text("Transfer Port: %u", static_cast<unsigned int>(networkDiagnostics_.transferPort));
    ImGui::Text("Multicast Group: %s", networkDiagnostics_.discoveryMulticastAddress.c_str());
    ImGui::Text("Local Name: %s", networkDiagnostics_.localDisplayName.c_str());
    ImGui::Text("Local Peer ID: %s", networkDiagnostics_.localPeerId.c_str());
    if (networkDiagnostics_.localIpv4Addresses.empty()) {
        ImGui::TextUnformatted("Local IPv4: -");
    } else {
        for (const auto& localIp : networkDiagnostics_.localIpv4Addresses) {
            ImGui::Text("Local IPv4: %s", localIp.c_str());
        }
    }
    if (networkDiagnostics_.directedBroadcastAddresses.empty()) {
        ImGui::TextUnformatted("Directed Broadcast Targets: -");
    } else {
        for (const auto& directedBroadcast : networkDiagnostics_.directedBroadcastAddresses) {
            ImGui::Text("Directed Broadcast Target: %s", directedBroadcast.c_str());
        }
    }
    ImGui::Text("Unicast Probe Targets: %zu", networkDiagnostics_.unicastProbeTargetCount);

    const double nowSeconds = networkDiagnostics_.nowSeconds;
    const double helloSentAge = AgeSeconds(nowSeconds, networkDiagnostics_.lastHelloSentSeconds);
    const double helloRecvAge = AgeSeconds(nowSeconds, networkDiagnostics_.lastHelloReceivedSeconds);
    const double udpSentAge = AgeSeconds(nowSeconds, networkDiagnostics_.lastUdpSentSeconds);
    const double udpRecvAge = AgeSeconds(nowSeconds, networkDiagnostics_.lastUdpReceivedSeconds);

    ImGui::Separator();
    DrawNetworkAgeRow("Last HELLO Sent:", helloSentAge);
    DrawNetworkAgeRow("Last HELLO Received:", helloRecvAge);
    DrawNetworkAgeRow("Last UDP Sent:", udpSentAge);
    DrawNetworkAgeRow("Last UDP Received:", udpRecvAge);
    ImGui::Text("Last UDP Sender: %s",
                networkDiagnostics_.lastUdpSenderIp.empty() ? "-" : networkDiagnostics_.lastUdpSenderIp.c_str());

    ImGui::Separator();
    ImGui::Text("UDP Sent: %llu", static_cast<unsigned long long>(networkDiagnostics_.udpPacketsSent));
    ImGui::Text("UDP Send Failures: %llu", static_cast<unsigned long long>(networkDiagnostics_.udpPacketsSendFailed));
    ImGui::Text("UDP Received: %llu", static_cast<unsigned long long>(networkDiagnostics_.udpPacketsReceived));
    ImGui::Text("HELLO Sent: %llu", static_cast<unsigned long long>(networkDiagnostics_.helloSentCount));
    ImGui::Text("HELLO Received: %llu", static_cast<unsigned long long>(networkDiagnostics_.helloReceivedCount));
    ImGui::Text("Offers Sent: %llu", static_cast<unsigned long long>(networkDiagnostics_.offersSentCount));
    ImGui::Text("Offers Received: %llu", static_cast<unsigned long long>(networkDiagnostics_.offersReceivedCount));

    ImGui::Separator();
    ImGui::Text("Peers Known: %zu", networkDiagnostics_.peersKnown);
    ImGui::Text("Peer Entries in UI: %zu", networkPeers_.size());
    ImGui::Text("Pending Incoming Offers: %zu", networkDiagnostics_.pendingIncomingOffers);
    ImGui::Text("Pending Outgoing UDP Packets: %zu", networkDiagnostics_.pendingOutgoingPackets);
    ImGui::Text("Cached Shared Payloads: %zu", networkDiagnostics_.cachedSharedPayloads);

    ImGui::Separator();
    ImGui::Text("Outgoing Fetch Attempts: %llu", static_cast<unsigned long long>(networkDiagnostics_.outgoingFetchAttempts));
    ImGui::Text("Outgoing Fetch Successes: %llu", static_cast<unsigned long long>(networkDiagnostics_.outgoingFetchSuccesses));
    ImGui::Text("Outgoing Fetch Failures: %llu", static_cast<unsigned long long>(networkDiagnostics_.outgoingFetchFailures));
    ImGui::Text("Incoming Transfer Requests: %llu", static_cast<unsigned long long>(networkDiagnostics_.incomingTransferRequests));
    ImGui::Text("Incoming Transfer Successes: %llu", static_cast<unsigned long long>(networkDiagnostics_.incomingTransferSuccesses));
    ImGui::Text("Incoming Transfer Failures: %llu", static_cast<unsigned long long>(networkDiagnostics_.incomingTransferFailures));

#if defined(_WIN32)
    ImGui::Separator();
    ImGui::Text("Winsock Initialized: %s", YesNo(networkDiagnostics_.winsockInitialized));
#endif

    ImGui::Separator();
    if (networkDiagnostics_.lastError.empty()) {
        ImGui::TextUnformatted("Last Error: -");
    } else {
        ImGui::TextWrapped("Last Error: %s", networkDiagnostics_.lastError.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Allow LAN Ports Through Firewall...")) {
        openFirewallAccessPopup_ = true;
    }
    if (openFirewallAccessPopup_) {
        ImGui::OpenPopup("Allow LAN Firewall Access");
        openFirewallAccessPopup_ = false;
    }
    if (ImGui::BeginPopupModal("Allow LAN Firewall Access", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Allow this app to automatically open LAN sharing ports (UDP 39541, TCP 39542) "
                           "in your system firewall?");
        ImGui::Separator();
        if (ImGui::Button("Allow")) {
            if (onRequestFirewallAccess_) {
                onRequestFirewallAccess_();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginTable("NetworkPeersDiagnosticsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("IP");
        ImGui::TableSetupColumn("Peer ID");
        ImGui::TableHeadersRow();
        for (const auto& peer : networkPeers_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(peer.displayName.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(peer.ipAddress.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(peer.id.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
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

    std::string workspaceLabel;
    if (const auto activeWorkspaceIt = std::ranges::find(workspaceNamesSnapshot, currentWorkspace);
        activeWorkspaceIt != workspaceNamesSnapshot.end()) {
        const auto activeWorkspaceIndex = static_cast<std::size_t>(
            std::distance(workspaceNamesSnapshot.begin(), activeWorkspaceIt));
        workspaceLabel = "Workspace " + std::to_string(activeWorkspaceIndex + 1) + ": " + currentWorkspace;
    } else {
        workspaceLabel = "Workspace: " + currentWorkspace;
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
    OpenShareWorkspacePopupIfRequested();
    DrawShareWorkspacePopup();

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

    const bool isPendingSelection =
        !pendingDocumentSelectionPath_.empty() && doc.filepath == pendingDocumentSelectionPath_;
    const bool isActiveDocument = (doc.filepath == activeDocumentPath_);

    ImGuiTabItemFlags tabFlags = doc.isDirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
    if (isPendingSelection) {
        *pendingSelectionVisible = true;
        tabFlags |= ImGuiTabItemFlags_SetSelected;
    }

    if (!ImGui::BeginTabItem(doc.filename.c_str(), &doc.isOpen, tabFlags)) {
        return true;
    }

    if (focusEditorRequestFramesRemaining_ > 0 && isActiveDocument) {
        // We set the focus to the next item (the child window inside TextEditor::Render)
        ImGui::SetKeyboardFocusHere();
    }

    if (isPendingSelection) {
        *pendingSelectionConsumed = true;
    }
    if (!isActiveDocument) {
        activeDocumentPath_ = doc.filepath;
        if (onActiveDocumentChanged_) {
            onActiveDocumentChanged_(doc.filepath, doc.lastKnownText);
        }
    }

    doc.editor.Render(doc.filename.c_str());
    // isActiveDocument captured before the assignment above is still authoritative for the
    // "previously active" check here — the focus-frame decrement is keyed off the same tab.
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
    const double autosaveDelaySeconds =
        std::max(0.05, static_cast<double>(activeSettings_.autosaveDelayMs) / 1000.0);
    for (auto& doc : openDocuments) {
        if (!doc.isDirty || (currentTime - doc.lastModifiedTime) < autosaveDelaySeconds) {
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

    // Capture the editor pane's bounds while it's still the active window — used to
    // anchor the U/P toggle overlay to this pane's top-right corner each frame.
    const ImGuiViewport* parentViewport = ImGui::GetWindowViewport();
    const ImVec2 codeEditorPos  = ImGui::GetWindowPos();
    const ImVec2 codeEditorSize = ImGui::GetWindowSize();
    ImGui::End();

    DrawEditorPaneToggleOverlay(parentViewport, codeEditorPos, codeEditorSize);
}

void EditorUI::DrawEditorPaneToggleOverlay(const ImGuiViewport* parentViewport,
                                           const ImVec2& paneTopLeft,
                                           const ImVec2& paneSize) {
    if (paneSize.x <= 0.0f || paneSize.y <= 0.0f) {
        return;
    }

    const float margin = 6.0f * currentDpiScale_;
    const ImVec2 anchorPos(paneTopLeft.x + paneSize.x - margin,
                           paneTopLeft.y + margin);
    if (parentViewport != nullptr) {
        ImGui::SetNextWindowViewport(parentViewport->ID);
    }
    ImGui::SetNextWindowPos(anchorPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    constexpr ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration |
                                               ImGuiWindowFlags_AlwaysAutoResize |
                                               ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoDocking |
                                               ImGuiWindowFlags_NoFocusOnAppearing |
                                               ImGuiWindowFlags_NoNav |
                                               ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##CodeEditorPaneToggles", nullptr, kOverlayFlags)) {
        const ImVec4 highlight = IsLightTheme() ? ImVec4(0.62f, 0.74f, 0.92f, 1.0f)
                                                 : ImVec4(0.30f, 0.50f, 0.78f, 1.0f);
        const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());

        // Snapshot the flag BEFORE the button — the click may flip it, and the matching
        // PopStyleColor must agree with the matching Push.
        const bool uniformsActive = showUniformControlsPanel_;
        if (uniformsActive) ImGui::PushStyleColor(ImGuiCol_Button, highlight);
        if (ImGui::Button("U", buttonSize)) {
            showUniformControlsPanel_ = !showUniformControlsPanel_;
        }
        if (uniformsActive) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Uniform Controls");

        ImGui::SameLine();
        const bool playbackActive = showPlaybackControlsPanel_;
        if (playbackActive) ImGui::PushStyleColor(ImGuiCol_Button, highlight);
        if (ImGui::Button("P", buttonSize)) {
            showPlaybackControlsPanel_ = !showPlaybackControlsPanel_;
        }
        if (playbackActive) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Playback Controls");
    }
    ImGui::End();
}
void EditorUI::AppendWorkspaceOutputLine(const std::string& text, bool isConsoleChannel) {
    std::string workspaceName;
    {
        std::scoped_lock workspaceLock(workspaceMutex_);
        workspaceName = activeWorkspaceName_;
    }
    if (workspaceName.empty()) {
        workspaceName = FALLBACK_WORKSPACE_NAME;
    }

    const std::size_t maxLines = isConsoleChannel ? MAX_CONSOLE_LINES : MAX_LOG_LINES;
    if (isConsoleChannel) {
        std::scoped_lock lock(consoleMutex);
        auto& lines = workspaceConsoleLines_[workspaceName];
        lines.push_back(text);
        if (lines.size() > maxLines) {
            const std::size_t removeCount = lines.size() - maxLines;
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(removeCount));
        }
        consoleScrollToBottom = true;
    } else {
        std::scoped_lock lock(logMutex);
        auto& lines = workspaceLogLines_[workspaceName];
        lines.push_back(text);
        if (lines.size() > maxLines) {
            const std::size_t removeCount = lines.size() - maxLines;
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(removeCount));
        }
        logScrollToBottom = true;
    }

    if (onWorkspaceLineAppended_) {
        onWorkspaceLineAppended_(workspaceName, text, isConsoleChannel);
    }
}

void EditorUI::AddConsoleOutput(const std::string& text) {
    AppendWorkspaceOutputLine(text, true);
}

void EditorUI::AddLogOutput(const std::string& text) {
    AppendWorkspaceOutputLine(text, false);
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

        DrawInputCaptureToolbar();
        TextureLayout::PlacedImage placedImage;
        TextureLayout::DrawCentered(rendererTexture_,
                                    rendererTextureWidth_,
                                    rendererTextureHeight_,
                                    "Renderer not ready.",
                                    &placedImage);
        UpdateRendererViewportStateFromImage(placedImage.drawn,
                                             placedImage.screenMin.x, placedImage.screenMin.y,
                                             placedImage.screenMax.x, placedImage.screenMax.y);
        DrawInputCaptureIndicator(placedImage.drawn,
                                  placedImage.screenMin.x, placedImage.screenMin.y,
                                  placedImage.screenMax.x, placedImage.screenMax.y);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void EditorUI::DrawRendererTab() {
    if (!ImGui::BeginTabItem("Renderer")) {
        return;
    }
    rendererTabActive_ = true;

    DrawInputCaptureToolbar();

    TextureLayout::PlacedImage placedImage;
    TextureLayout::DrawCentered(rendererTexture_,
                                rendererTextureWidth_,
                                rendererTextureHeight_,
                                "Renderer not ready.",
                                &placedImage);
    UpdateRendererViewportStateFromImage(placedImage.drawn,
                                         placedImage.screenMin.x, placedImage.screenMin.y,
                                         placedImage.screenMax.x, placedImage.screenMax.y);
    DrawInputCaptureIndicator(placedImage.drawn,
                              placedImage.screenMin.x, placedImage.screenMin.y,
                              placedImage.screenMax.x, placedImage.screenMax.y);

    ImGui::EndTabItem();
}

void EditorUI::DrawInputCaptureToolbar() {
    // Single-row toolbar above the rendered image so the toggle is always
    // discoverable and right next to the thing it affects.
    const bool previous = inputCaptureEnabled_;
    bool checkboxValue = previous;
    if (ImGui::Checkbox("Capture Input", &checkboxValue) && checkboxValue != previous) {
        inputCaptureEnabled_ = checkboxValue;
        if (onInputCaptureEnabledChanged_) {
            onInputCaptureEnabledChanged_(checkboxValue);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(F1 toggles)");
    if (inputCaptureEnabled_) {
        ImGui::SameLine();
        const ImVec4 badgeColor = inputCaptureActive_ ? ImVec4(0.18f, 0.78f, 0.34f, 1.0f)
                                                       : ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
        ImGui::TextColored(badgeColor, "%s", inputCaptureActive_ ? "INPUT ACTIVE" : "input idle");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              inputCaptureActive_
                                  ? "JIT scene is receiving keyboard / mouse input."
                                  : "Capture is enabled but the renderer panel is not focused, "
                                    "or ImGui is consuming input. Click the image to focus.");
        }
    }
}

void EditorUI::UpdateRendererViewportStateFromImage(bool drawn,
                                                    float minX, float minY,
                                                    float maxX, float maxY) {
    RendererViewportState state{};
    state.visible = drawn;
    if (drawn) {
        state.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImVec2 mouse = ImGui::GetMousePos();
        state.hovered = (mouse.x >= minX && mouse.x < maxX && mouse.y >= minY && mouse.y < maxY);
        state.screenMinX = minX;
        state.screenMinY = minY;
        state.screenMaxX = maxX;
        state.screenMaxY = maxY;
        state.textureWidth = rendererTextureWidth_;
        state.textureHeight = rendererTextureHeight_;
    }
    pendingRendererViewportState_ = state;
    rendererViewportState_ = state;
}

void EditorUI::DrawInputCaptureIndicator(bool drawn,
                                         float minX, float minY,
                                         float maxX, float maxY) {
    if (!drawn || !inputCaptureEnabled_) {
        return;
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (drawList == nullptr) {
        return;
    }
    const ImU32 color = inputCaptureActive_
                            ? IM_COL32(46, 200, 87, 230)
                            : IM_COL32(217, 167, 51, 200);
    const float thickness = inputCaptureActive_ ? 2.5f : 1.5f;
    drawList->AddRect(ImVec2(minX - 1.0f, minY - 1.0f),
                      ImVec2(maxX + 1.0f, maxY + 1.0f),
                      color,
                      0.0f,
                      0,
                      thickness);
}

void EditorUI::SetInputCaptureEnabled(bool enabled) {
    inputCaptureEnabled_ = enabled;
}

void EditorUI::DrawPipelineTab() {
    if (!ImGui::BeginTabItem("Pipeline")) {
        return;
    }

    std::vector<std::string> disabledPasses;
    disabledPasses.reserve(pipelinePasses_.size());
    for (const auto& pass : pipelinePasses_) {
        if (!pass.enabled) {
            disabledPasses.push_back(pass.workspaceName);
        }
    }
    if (selectedPipelineAddPassIndex_ >= static_cast<int>(disabledPasses.size())) {
        selectedPipelineAddPassIndex_ = 0;
    }

    ImGui::PushID("PipelineToolbar");
    const ImGuiStyle& style = ImGui::GetStyle();
    const float toolbarButtonWidth = 108.0f;
    const float addButtonWidth = 56.0f;
    const float saveButtonWidth = 90.0f;
    const float resetButtonWidth = 74.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float comboWidth = std::clamp(availableWidth * 0.45f, 120.0f, 280.0f);

    if (disabledPasses.empty()) {
        ImGui::BeginDisabled();
    }
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("Add Pass", disabledPasses.empty() ? "None" :
                          disabledPasses[static_cast<std::size_t>(selectedPipelineAddPassIndex_)].c_str())) {
        for (std::size_t i = 0; i < disabledPasses.size(); ++i) {
            const bool selected = (selectedPipelineAddPassIndex_ == static_cast<int>(i));
            if (ImGui::Selectable(disabledPasses[i].c_str(), selected)) {
                selectedPipelineAddPassIndex_ = static_cast<int>(i);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::GetContentRegionAvail().x > (addButtonWidth + style.ItemSpacing.x)) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Add", ImVec2(addButtonWidth, 0.0f)) &&
        !disabledPasses.empty() &&
        onPipelineAddPass_) {
        onPipelineAddPass_(disabledPasses[static_cast<std::size_t>(selectedPipelineAddPassIndex_)]);
    }
    if (disabledPasses.empty()) {
        ImGui::EndDisabled();
    }

    if (ImGui::GetContentRegionAvail().x > (toolbarButtonWidth + style.ItemSpacing.x)) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Global Uniforms", ImVec2(toolbarButtonWidth, 0.0f))) {
        showGlobalUniformsWindow_ = !showGlobalUniformsWindow_;
    }

    if (ImGui::GetContentRegionAvail().x > (saveButtonWidth + style.ItemSpacing.x)) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Save Chain", ImVec2(saveButtonWidth, 0.0f))) {
        nfdchar_t* outPath = nullptr;
        nfdfilteritem_t filterItem[1] = {{"JITGL Chain", "jchain"}};
        const nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, 1, nullptr, "pipeline.jchain");
        if (result == NFD_OKAY) {
            const bool saved = onPipelineSaveChain_ ? onPipelineSaveChain_(outPath) : false;
            AddLogOutput(saved ? "[Pipeline] Chain saved." : "[Pipeline Error] Failed to save chain.");
            NFD_FreePath(outPath);
        }
    }
    if (ImGui::GetContentRegionAvail().x > (resetButtonWidth + style.ItemSpacing.x)) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Reset", ImVec2(resetButtonWidth, 0.0f)) && onPipelineReset_) {
        onPipelineReset_();
    }
    ImGui::PopID();

    std::vector<std::string> enabledPasses;
    enabledPasses.reserve(pipelinePasses_.size());
    for (const auto& pass : pipelinePasses_) {
        if (pass.enabled) {
            enabledPasses.push_back(pass.workspaceName);
        }
    }
    const std::string orderText = BuildPipelineOrderText(enabledPasses);

    ImGui::Separator();
    DrawPipelineInspectorPanel(orderText);
    DrawPipelineGlobalUniformWindow();

    ImGui::EndTabItem();
}

std::string EditorUI::BuildPipelineOrderText(const std::vector<std::string>& enabledPasses) const {
    if (enabledPasses.empty()) {
        return "(none)";
    }

    std::string orderText;
    for (std::size_t i = 0; i < enabledPasses.size(); ++i) {
        if (i > 0) {
            orderText += " -> ";
        }
        orderText += std::to_string(i + 1);
        orderText += ":";
        orderText += enabledPasses[i];
    }
    return orderText;
}

void EditorUI::DrawPipelineInspectorPanel(const std::string& orderText) {
    if (!ImGui::BeginChild("PipelineStack", ImVec2(0.0f, 0.0f), false)) {
        ImGui::EndChild();
        return;
    }

    ImGui::TextUnformatted("Execution Order");
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(orderText.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const std::vector<PipelinePassView> passesSnapshot = pipelinePasses_;
    const std::vector<PipelineResourceView> resourcesSnapshot = pipelineResources_;

    std::unordered_map<std::string, const PipelineResourceView*> resourcesByName;
    resourcesByName.reserve(resourcesSnapshot.size());
    for (const auto& resource : resourcesSnapshot) {
        auto it = resourcesByName.find(resource.name);
        if (it == resourcesByName.end()) {
            resourcesByName.emplace(resource.name, &resource);
            continue;
        }
        if ((it->second->texture == 0 || it->second->width <= 0 || it->second->height <= 0) &&
            resource.texture != 0 && resource.width > 0 && resource.height > 0) {
            it->second = &resource;
        }
    }

    const auto resolvePassPreview = [&](const PipelinePassView& pass) -> const PipelineResourceView* {
        const std::string preferredOutput = pass.outputName.empty() ? pass.workspaceName : pass.outputName;
        if (const auto it = resourcesByName.find(preferredOutput); it != resourcesByName.end()) {
            return it->second;
        }
        if (const auto it = resourcesByName.find(pass.workspaceName); it != resourcesByName.end()) {
            return it->second;
        }
        return nullptr;
    };

    for (std::size_t i = 0; i < passesSnapshot.size(); ++i) {
        const auto& pass = passesSnapshot[i];
        ImGui::PushID(pass.workspaceName.c_str());

        ImGui::BeginGroup();
        const bool cardDisabled = !pass.enabled;
        if (cardDisabled) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.58f);
        }
        const PipelineResourceView* preview = resolvePassPreview(pass);
        const float previewWidth = std::clamp(ImGui::GetContentRegionAvail().x - 20.0f, 120.0f, 360.0f);
        float previewHeight = 0.0f;
        if (preview != nullptr && preview->texture != 0 && preview->width > 0 && preview->height > 0) {
            const float aspect = static_cast<float>(preview->width) /
                                 std::max(1.0f, static_cast<float>(preview->height));
            previewHeight = std::clamp(previewWidth / std::max(0.01f, aspect), 72.0f, 200.0f);
        }
        const float cardHeight = std::max(126.0f, previewHeight + 96.0f);
        const bool cardOpen = ImGui::BeginChild("PipelinePassCard", ImVec2(0.0f, cardHeight), true);

        if (cardOpen) {

            const bool canMoveUp = i > 0;
            if (!canMoveUp) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("Up") && onPipelineMove_) {
                const std::string workspaceName = pass.workspaceName;
                onPipelineMove_(workspaceName, -1);
            }
            if (!canMoveUp) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            const bool canMoveDown = (i + 1 < passesSnapshot.size());
            if (!canMoveDown) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("Down") && onPipelineMove_) {
                const std::string workspaceName = pass.workspaceName;
                onPipelineMove_(workspaceName, +1);
            }
            if (!canMoveDown) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            bool enabled = pass.enabled;
            if (ImGui::Checkbox("##Enabled", &enabled) && onPipelineEdit_) {
                PipelineEditCommand command;
                command.action = PipelineEditAction::SetEnabled;
                command.workspaceName = pass.workspaceName;
                command.enabled = enabled;
                command.outputName = pass.outputName;
                onPipelineEdit_(command);
            }

            ImGui::SameLine();
            ImGui::Text("%zu. %s", i + 1, pass.workspaceName.c_str());

            const char* statusText = pass.compiled ? (pass.hasCompileError ? "Error" : "Ready") : "No Program";
            const ImVec4 statusColor = pass.compiled && !pass.hasCompileError
                                           ? (IsLightTheme() ? ImVec4(0.16f, 0.56f, 0.24f, 1.0f)
                                                             : ImVec4(0.45f, 1.0f, 0.45f, 1.0f))
                                           : (IsLightTheme() ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f)
                                                             : ImVec4(1.0f, 0.38f, 0.38f, 1.0f));
            ImGui::TextColored(statusColor, "Status: %s", statusText);
            ImGui::SameLine();
            ImGui::TextDisabled("GPU: %.3f ms", pass.gpuTimeMs);

            if (preview != nullptr && preview->texture != 0 && preview->width > 0 && preview->height > 0) {
                ImGui::Image(static_cast<ImTextureID>(preview->texture),
                             ImVec2(previewWidth, previewHeight),
                             ImVec2(0.0f, 1.0f),
                             ImVec2(1.0f, 0.0f));
            } else {
                ImGui::TextDisabled("Preview unavailable");
            }

            if (!pass.cppPath.empty()) {
                if (ImGui::SmallButton("scene.cpp") && onPipelineOpenFile_) {
                    const std::string workspaceName = pass.workspaceName;
                    onPipelineOpenFile_(workspaceName, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", pass.cppPath.c_str());
                }
            }
            if (!pass.shaderPath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("shader.glsl") && onPipelineOpenFile_) {
                    const std::string workspaceName = pass.workspaceName;
                    onPipelineOpenFile_(workspaceName, false);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", pass.shaderPath.c_str());
                }
            }
        }

        ImGui::EndChild();
        if (cardDisabled) {
            ImGui::PopStyleVar();
        }
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::EndChild();
}

void EditorUI::DrawPipelineGlobalUniformWindow() {
    if (!showGlobalUniformsWindow_) {
        return;
    }

    bool open = showGlobalUniformsWindow_;
    if (ImGui::Begin("Global Uniforms", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        static std::array<char, 96> newUniformName{};
        static int newUniformType = 0;

        ImGui::InputText("Name", newUniformName.data(), newUniformName.size());
        const std::array<const char*, 4> uniformTypes = {"float", "int", "bool", "vec4"};
        ImGui::Combo("Type", &newUniformType, uniformTypes.data(), static_cast<int>(uniformTypes.size()));
        if (ImGui::Button("Add Global")) {
            const std::string uniformName = TrimString(newUniformName.data());
            if (!uniformName.empty() && onPipelineGlobalUniform_) {
                PipelineGlobalUniformCommand command;
                command.action = PipelineGlobalUniformCommand::Action::Upsert;
                command.uniform.name = uniformName;
                command.uniform.type = static_cast<PipelineGlobalUniformType>(newUniformType);
                command.uniform.floatValue = 0.0f;
                command.uniform.intValue = 0;
                command.uniform.boolValue = false;
                command.uniform.vec4Value = {0.0f, 0.0f, 0.0f, 0.0f};
                onPipelineGlobalUniform_(command);
                newUniformName[0] = '\0';
            }
        }

        ImGui::Separator();
        if (pipelineGlobalUniforms_.empty()) {
            ImGui::TextDisabled("No global uniforms configured.");
        } else {
            for (std::size_t i = 0; i < pipelineGlobalUniforms_.size(); ++i) {
                auto uniform = pipelineGlobalUniforms_[i];
                ImGui::PushID(uniform.name.c_str());
                ImGui::Text("%s (%s)",
                            uniform.name.c_str(),
                            PipelineGlobalUniformTypeLabel(uniform.type));
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    if (onPipelineGlobalUniform_) {
                        PipelineGlobalUniformCommand command;
                        command.action = PipelineGlobalUniformCommand::Action::Remove;
                        command.uniform = uniform;
                        onPipelineGlobalUniform_(command);
                    }
                    ImGui::PopID();
                    continue;
                }

                bool changed = false;
                switch (uniform.type) {
                    case PipelineGlobalUniformType::Float:
                        changed = ImGui::DragFloat("Value", &uniform.floatValue, 0.01f, -10000.0f, 10000.0f);
                        break;
                    case PipelineGlobalUniformType::Int:
                        changed = ImGui::DragInt("Value", &uniform.intValue, 1.0f, -100000, 100000);
                        break;
                    case PipelineGlobalUniformType::Bool:
                        changed = ImGui::Checkbox("Value", &uniform.boolValue);
                        break;
                    case PipelineGlobalUniformType::Vec4:
                        changed = ImGui::DragFloat4("Value", uniform.vec4Value.data(), 0.01f, -10000.0f, 10000.0f);
                        break;
                }

                if (changed) {
                    pipelineGlobalUniforms_[i] = uniform;
                    if (onPipelineGlobalUniform_) {
                        PipelineGlobalUniformCommand command;
                        command.action = PipelineGlobalUniformCommand::Action::Upsert;
                        command.uniform = uniform;
                        onPipelineGlobalUniform_(command);
                    }
                }

                ImGui::Separator();
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
    showGlobalUniformsWindow_ = open;
}

void EditorUI::DrawPlaybackTransportBar() {
    if (!showPlaybackControlsPanel_) {
        return;
    }

    auto submitPlaybackCommand = [this](const PlaybackCommand& command) {
        if (onPlaybackCommand_) {
            onPlaybackCommand_(command);
        }
    };

    if (ImGui::Button(playbackState_.paused ? "Play" : "Pause")) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::TogglePause, 0.0f, false });
    }
    ImGui::SameLine();
    if (ImGui::Button("Rewind")) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::Rewind, 0.0f, false });
    }
    ImGui::SameLine();
    ImGui::Text("Time %.3fs", playbackState_.currentTimeSeconds);

    float timelineMax = std::max(1.0f, playbackState_.timelineMaxSeconds);
    if (timelineMax < playbackState_.loopEndSeconds) {
        timelineMax = playbackState_.loopEndSeconds;
    }

    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::DragFloat("Timeline End", &timelineMax, 0.1f, 1.0f, 3600.0f, "%.2fs")) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::SetTimelineMax, timelineMax, false });
    }

    const float scrubMin = playbackState_.loopEnabled
                               ? std::clamp(playbackState_.loopStartSeconds, 0.0f, timelineMax)
                               : 0.0f;
    const float scrubMax = playbackState_.loopEnabled
                               ? std::max(scrubMin + 0.01f,
                                          std::clamp(playbackState_.loopEndSeconds, scrubMin + 0.01f, timelineMax))
                               : timelineMax;
    float scrubTime = std::clamp(playbackState_.currentTimeSeconds, scrubMin, scrubMax);
    if (ImGui::SliderFloat("Time Scrubber", &scrubTime, scrubMin, scrubMax, "%.3fs")) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::SetTime, scrubTime, false });
    }

    ImGui::TextUnformatted("Speed");
    ImGui::SameLine();
    const std::array<std::pair<const char*, float>, 4> speeds = {{
        {"0.25x", 0.25f},
        {"0.5x", 0.5f},
        {"1x", 1.0f},
        {"2x", 2.0f},
    }};
    for (std::size_t i = 0; i < speeds.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        const bool selected = std::abs(playbackState_.speed - speeds[i].second) < 0.001f;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, IsLightTheme() ? ImVec4(0.75f, 0.82f, 0.92f, 1.0f)
                                                                  : ImVec4(0.22f, 0.37f, 0.53f, 1.0f));
        }
        if (ImGui::Button(speeds[i].first)) {
            submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::SetSpeed, speeds[i].second, false });
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
    }

    bool loopEnabled = playbackState_.loopEnabled;
    if (ImGui::Checkbox("Loop Range", &loopEnabled)) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::SetLoopEnabled, 0.0f, loopEnabled });
    }

    float loopStart = std::clamp(playbackState_.loopStartSeconds, 0.0f, timelineMax);
    float loopEnd = std::clamp(playbackState_.loopEndSeconds, 0.0f, std::max(timelineMax, loopStart + 0.01f));
    if (loopEnd <= loopStart) {
        loopEnd = loopStart + 0.01f;
    }
    loopEnd = std::clamp(loopEnd, 0.0f, std::max(timelineMax, loopStart + 0.01f));

    const float loopStartMax = std::max(0.0f, loopEnd - 0.01f);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::DragFloat("Loop Start", &loopStart, 0.05f, 0.0f, loopStartMax, "%.2fs")) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::SetLoopStart, loopStart, false });
    }

    const float loopEndMin = std::min(timelineMax, loopStart + 0.01f);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::DragFloat("Loop End", &loopEnd, 0.05f, loopEndMin, std::max(timelineMax, loopEndMin), "%.2fs")) {
        submitPlaybackCommand(PlaybackCommand{ PlaybackCommandType::SetLoopEnd, loopEnd, false });
    }

    ImGui::Separator();
}

void EditorUI::DrawUniformsTab() {
    if (!rendererFullscreen_ && !showUniformControlsPanel_ && !showPlaybackControlsPanel_) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 12.0f;
    const float topOffset = rendererFullscreen_ ? margin : 40.0f;
    float panelsY = topOffset + 8.0f;
    if (rendererFullscreen_) {
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin,
                                       viewport->WorkPos.y + topOffset),
                                ImGuiCond_Always,
                                ImVec2(1.0f, 0.0f));

        const ImGuiWindowFlags toggleFlags = ImGuiWindowFlags_NoDecoration |
                                             ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoDocking |
                                             ImGuiWindowFlags_NoNav;

        float toggleHeight = 0.0f;
        if (ImGui::Begin("UniformControlsToggle", nullptr, toggleFlags)) {
            const ImVec4 highlight = IsLightTheme() ? ImVec4(0.75f, 0.82f, 0.92f, 1.0f)
                                                     : ImVec4(0.22f, 0.37f, 0.53f, 1.0f);
            const ImVec2 toggleSize(ImGui::GetFrameHeight() * 1.4f, 0.0f);

            const bool uniformsActive = showUniformControlsPanel_;
            if (uniformsActive) ImGui::PushStyleColor(ImGuiCol_Button, highlight);
            if (ImGui::Button("U", toggleSize)) {
                showUniformControlsPanel_ = !showUniformControlsPanel_;
            }
            if (uniformsActive) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Uniform Controls");
            ImGui::SameLine();
            const bool playbackActive = showPlaybackControlsPanel_;
            if (playbackActive) ImGui::PushStyleColor(ImGuiCol_Button, highlight);
            if (ImGui::Button("P", toggleSize)) {
                showPlaybackControlsPanel_ = !showPlaybackControlsPanel_;
            }
            if (playbackActive) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Playback Controls");
            toggleHeight = ImGui::GetWindowSize().y;
        }
        ImGui::End();
        panelsY = topOffset + toggleHeight + 8.0f;
    }

    // Floating panels are movable and remember position across show/hide via ImGui's ini.
    // Pinned to the main viewport so they don't tear off into a separate OS-level viewport
    // (which was causing crashes on tear-off with multi-viewport enabled).
    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_AlwaysAutoResize;
    if (showUniformControlsPanel_) {
        bool panelOpen = showUniformControlsPanel_;
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin,
                                       viewport->WorkPos.y + panelsY),
                                ImGuiCond_FirstUseEver,
                                ImVec2(1.0f, 0.0f));

        if (ImGui::Begin("Uniform Controls", &panelOpen, panelFlags)) {

            auto submitEdit = [this](const UniformEditCommand& command) {
                if (onUniformEdit_) {
                    onUniformEdit_(command);
                }
            };

            auto defaultScalarForRange = [](float minValue, float maxValue) {
                if (maxValue < minValue) {
                    std::swap(minValue, maxValue);
                }
                if (0.0f < minValue) {
                    return minValue;
                }
                if (0.0f > maxValue) {
                    return maxValue;
                }
                return 0.0f;
            };

            auto valueToClipboardText = [](const UniformValue& value) {
                std::ostringstream out;
                switch (value.desc.type) {
                    case UniformType::Float:
                        out << value.f;
                        break;
                    case UniformType::Int:
                        out << value.i;
                        break;
                    case UniformType::Bool:
                        out << (value.b ? "true" : "false");
                        break;
                    case UniformType::Vec2:
                        out << "[" << value.v[0] << ", " << value.v[1] << "]";
                        break;
                    case UniformType::Vec3:
                        out << "[" << value.v[0] << ", " << value.v[1] << ", " << value.v[2] << "]";
                        break;
                    case UniformType::Vec4:
                        out << "[" << value.v[0] << ", " << value.v[1] << ", " << value.v[2] << ", " << value.v[3] << "]";
                        break;
                }
                return out.str();
            };

            auto drawUniformWidget = [&](std::size_t index) {
                UniformValue& value = uniformValues_[index];
                const std::string widgetLabel = value.desc.label.empty() ? value.desc.name : value.desc.label;
                const bool fineAdjust = ImGui::GetIO().KeyAlt;
                const float dragStep = value.desc.step > 0.0f ? value.desc.step : 0.01f;

                ImGui::PushID(value.desc.name.c_str());

                bool changed = false;
                switch (value.desc.type) {
                    case UniformType::Float: {
                        float edited = value.f;
                        if (fineAdjust || value.desc.step > 0.0f) {
                            changed = ImGui::DragFloat(widgetLabel.c_str(),
                                                       &edited,
                                                       dragStep,
                                                       value.desc.rangeMin,
                                                       value.desc.rangeMax);
                        } else {
                            changed = ImGui::SliderFloat(widgetLabel.c_str(), &edited, value.desc.rangeMin, value.desc.rangeMax);
                        }
                        if (changed) {
                            value.f = edited;
                            UniformEditCommand command;
                            command.action = UniformEditAction::SetFloat;
                            command.name = value.desc.name;
                            command.floatValue = value.f;
                            submitEdit(command);
                        }
                        break;
                    }
                    case UniformType::Int: {
                        if (value.desc.widget == UniformWidgetHint::Toggle) {
                            bool checked = (value.i != 0);
                            changed = ImGui::Checkbox(widgetLabel.c_str(), &checked);
                            if (changed) {
                                value.i = checked ? 1 : 0;
                                UniformEditCommand command;
                                command.action = UniformEditAction::SetBool;
                                command.name = value.desc.name;
                                command.boolValue = checked;
                                submitEdit(command);
                            }
                        } else {
                            int edited = value.i;
                            changed = ImGui::SliderInt(widgetLabel.c_str(),
                                                       &edited,
                                                       static_cast<int>(value.desc.rangeMin),
                                                       static_cast<int>(value.desc.rangeMax));
                            if (changed) {
                                value.i = edited;
                                UniformEditCommand command;
                                command.action = UniformEditAction::SetInt;
                                command.name = value.desc.name;
                                command.intValue = value.i;
                                submitEdit(command);
                            }
                        }
                        break;
                    }
                    case UniformType::Bool: {
                        bool edited = value.b;
                        changed = ImGui::Checkbox(widgetLabel.c_str(), &edited);
                        if (changed) {
                            value.b = edited;
                            UniformEditCommand command;
                            command.action = UniformEditAction::SetBool;
                            command.name = value.desc.name;
                            command.boolValue = value.b;
                            submitEdit(command);
                        }
                        break;
                    }
                    case UniformType::Vec2: {
                        std::array<float, 4> edited = value.v;
                        if (fineAdjust || value.desc.step > 0.0f) {
                            changed = ImGui::DragFloat2(widgetLabel.c_str(),
                                                        edited.data(),
                                                        dragStep,
                                                        value.desc.rangeMin,
                                                        value.desc.rangeMax);
                        } else {
                            changed = ImGui::SliderFloat2(widgetLabel.c_str(),
                                                          edited.data(),
                                                          value.desc.rangeMin,
                                                          value.desc.rangeMax);
                        }
                        if (changed) {
                            value.v = edited;
                            UniformEditCommand command;
                            command.action = UniformEditAction::SetVec;
                            command.name = value.desc.name;
                            command.vecValue = value.v;
                            command.components = 2;
                            submitEdit(command);
                        }
                        break;
                    }
                    case UniformType::Vec3: {
                        std::array<float, 4> edited = value.v;
                        if (value.desc.widget == UniformWidgetHint::Color) {
                            changed = ImGui::ColorEdit3(widgetLabel.c_str(), edited.data(), ImGuiColorEditFlags_Float);
                        } else if (fineAdjust || value.desc.step > 0.0f) {
                            changed = ImGui::DragFloat3(widgetLabel.c_str(),
                                                        edited.data(),
                                                        dragStep,
                                                        value.desc.rangeMin,
                                                        value.desc.rangeMax);
                        } else {
                            changed = ImGui::SliderFloat3(widgetLabel.c_str(),
                                                          edited.data(),
                                                          value.desc.rangeMin,
                                                          value.desc.rangeMax);
                        }
                        if (changed) {
                            value.v = edited;
                            UniformEditCommand command;
                            command.action = UniformEditAction::SetVec;
                            command.name = value.desc.name;
                            command.vecValue = value.v;
                            command.components = 3;
                            submitEdit(command);
                        }
                        break;
                    }
                    case UniformType::Vec4: {
                        std::array<float, 4> edited = value.v;
                        if (value.desc.widget == UniformWidgetHint::Color) {
                            changed = ImGui::ColorEdit4(widgetLabel.c_str(), edited.data(), ImGuiColorEditFlags_Float);
                        } else if (fineAdjust || value.desc.step > 0.0f) {
                            changed = ImGui::DragFloat4(widgetLabel.c_str(),
                                                        edited.data(),
                                                        dragStep,
                                                        value.desc.rangeMin,
                                                        value.desc.rangeMax);
                        } else {
                            changed = ImGui::SliderFloat4(widgetLabel.c_str(),
                                                          edited.data(),
                                                          value.desc.rangeMin,
                                                          value.desc.rangeMax);
                        }
                        if (changed) {
                            value.v = edited;
                            UniformEditCommand command;
                            command.action = UniformEditAction::SetVec;
                            command.name = value.desc.name;
                            command.vecValue = value.v;
                            command.components = 4;
                            submitEdit(command);
                        }
                        break;
                    }
                }

                if (ImGui::BeginPopupContextItem("UniformContextMenu")) {
                    if (ImGui::MenuItem("Reset")) {
                        UniformEditCommand command;
                        command.action = UniformEditAction::ResetOne;
                        command.name = value.desc.name;
                        submitEdit(command);

                        const float defaultScalar = defaultScalarForRange(value.desc.rangeMin, value.desc.rangeMax);
                        value.f = defaultScalar;
                        value.i = static_cast<int>(std::lround(defaultScalar));
                        value.b = false;
                        value.v = { defaultScalar, defaultScalar, defaultScalar, defaultScalar };
                    }
                    if (ImGui::MenuItem("Copy Value")) {
                        const std::string valueText = valueToClipboardText(value);
                        ImGui::SetClipboardText(valueText.c_str());
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            };

            std::vector<std::string> groupOrder;
            std::unordered_map<std::string, std::vector<std::size_t>> groupedIndices;
            std::vector<std::size_t> ungroupedIndices;
            bool hasVisibleUniform = false;

            for (std::size_t i = 0; i < uniformValues_.size(); ++i) {
                const UniformValue& value = uniformValues_[i];
                if (value.desc.hidden) {
                    continue;
                }
                hasVisibleUniform = true;
                if (value.desc.group.empty()) {
                    ungroupedIndices.push_back(i);
                    continue;
                }
                if (!groupedIndices.contains(value.desc.group)) {
                    groupOrder.push_back(value.desc.group);
                }
                groupedIndices[value.desc.group].push_back(i);
            }

            if (!hasVisibleUniform) {
                ImGui::TextDisabled("No discoverable uniforms in shader.glsl.");
            } else {
                for (const auto& groupName : groupOrder) {
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
                    if (ImGui::CollapsingHeader(groupName.c_str(), flags)) {
                        for (const std::size_t index : groupedIndices[groupName]) {
                            drawUniformWidget(index);
                        }
                    }
                }
                for (const std::size_t index : ungroupedIndices) {
                    drawUniformWidget(index);
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Reset All")) {
                UniformEditCommand resetAllCommand;
                resetAllCommand.action = UniformEditAction::ResetAll;
                submitEdit(resetAllCommand);
                for (auto& value : uniformValues_) {
                    const float defaultScalar = defaultScalarForRange(value.desc.rangeMin, value.desc.rangeMax);
                    value.f = defaultScalar;
                    value.i = static_cast<int>(std::lround(defaultScalar));
                    value.b = false;
                    value.v = { defaultScalar, defaultScalar, defaultScalar, defaultScalar };
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Copy JSON")) {
                const std::string json = onUniformJsonSnapshot_ ? onUniformJsonSnapshot_() : std::string("{}\n");
                ImGui::SetClipboardText(json.c_str());
            }
        }

        showUniformControlsPanel_ = panelOpen;
        panelsY += ImGui::GetWindowSize().y + 8.0f;
        ImGui::End();
    }

    if (showPlaybackControlsPanel_) {
        bool panelOpen = showPlaybackControlsPanel_;
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin,
                                       viewport->WorkPos.y + panelsY),
                                ImGuiCond_FirstUseEver,
                                ImVec2(1.0f, 0.0f));
        if (ImGui::Begin("Playback Controls", &panelOpen, panelFlags)) {
            DrawPlaybackTransportBar();
        }
        showPlaybackControlsPanel_ = panelOpen;
        ImGui::End();
    }
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

    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float labelWidth = ImGui::CalcTextSize("Render:").x;
        const float workspaceWidth = ImGui::CalcTextSize("Workspace").x + style.FramePadding.x * 2.0f +
                                     ImGui::GetFrameHeight();
        const float pipelineWidth = ImGui::CalcTextSize("Pipeline").x + style.FramePadding.x * 2.0f +
                                    ImGui::GetFrameHeight();
        const float controlsWidth = labelWidth + style.ItemInnerSpacing.x + workspaceWidth + style.ItemSpacing.x +
                                    pipelineWidth;
        const float startX = std::max(0.0f, ImGui::GetWindowContentRegionMax().x - controlsWidth);
        ImGui::SetCursorPosX(startX);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Render:");
        ImGui::SameLine();

        if (ImGui::RadioButton("Workspace", rendererOutputMode_ == RendererOutputMode::ActiveWorkspace)) {
            rendererOutputMode_ = RendererOutputMode::ActiveWorkspace;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Pipeline", rendererOutputMode_ == RendererOutputMode::PipelineChain)) {
            rendererOutputMode_ = RendererOutputMode::PipelineChain;
        }
        // Uniform / Playback toggles live in the code-editor tab bar (top-right).
    }

    const std::string currentWorkspace = ResolveCurrentWorkspaceName();
    if (ImGui::BeginTabBar("UtilityTabs", ImGuiTabBarFlags_None)) {
        DrawRendererTab();
        DrawPipelineTab();
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

// ─────────────────────────────────────────────────────────────────────────────
// Settings persistence + apply
// ─────────────────────────────────────────────────────────────────────────────

void EditorUI::SetAppSettingsAppliedCallback(std::function<void(const AppSettings&)> cb) {
    onAppSettingsApplied_ = std::move(cb);
}

void EditorUI::ReportFrameTime(float frameMilliseconds) {
    constexpr int kCapacity = static_cast<int>(sizeof(frameTimeSamplesMs_) / sizeof(frameTimeSamplesMs_[0]));
    frameTimeSamplesMs_[frameTimeSampleIndex_] = frameMilliseconds;
    frameTimeSampleIndex_ = (frameTimeSampleIndex_ + 1) % kCapacity;
    if (frameTimeSampleCount_ < kCapacity) {
        ++frameTimeSampleCount_;
    }
}

namespace {
    constexpr int kAllowedUiScales[] = { 75, 100, 125, 150, 200 };

    int NearestAllowedScale(int percent) {
        int best = kAllowedUiScales[0];
        int bestDist = std::abs(percent - best);
        for (const int candidate : kAllowedUiScales) {
            const int dist = std::abs(percent - candidate);
            if (dist < bestDist) {
                best = candidate;
                bestDist = dist;
            }
        }
        return best;
    }
}

void EditorUI::LoadAllSettingsFromPrefs() {
    AppSettings loaded;
    loaded.vsyncEnabled       = appPreferences_.GetBool("graphics.vsync", true);
    loaded.targetFramerate    = std::clamp(appPreferences_.GetInt("graphics.target_fps", 60), 15, 480);
    loaded.showFpsOverlay     = appPreferences_.GetBool("graphics.show_fps_overlay", false);
    loaded.fpsOverlayCorner   = std::clamp(appPreferences_.GetInt("graphics.fps_overlay_corner", 1), 0, 3);
    const std::string themeStr = appPreferences_.GetString("appearance.theme", "dark");
    loaded.darkTheme          = (themeStr != "light");
    loaded.uiScalePercent     = NearestAllowedScale(appPreferences_.GetInt("appearance.ui_scale_percent",
                                                                            static_cast<int>(currentDpiScale_ * 100.0f)));
    loaded.autosaveDelayMs    = std::clamp(appPreferences_.GetInt("editor.autosave_delay_ms", 1000), 250, 10000);
    loaded.showWelcomeOnStartup  = showWelcomeOnStartup_;
    loaded.loadShowcaseOnStartup = loadShowcaseWorkspaceOnStartup_;
    loaded.networkEnabled     = networkEnabled_;

    ApplyAppSettings(loaded, /*dispatchCallback=*/false);
}

void EditorUI::SaveAllSettingsToPrefs() {
    appPreferences_.SetBool ("graphics.vsync",                activeSettings_.vsyncEnabled);
    appPreferences_.SetInt  ("graphics.target_fps",           activeSettings_.targetFramerate);
    appPreferences_.SetBool ("graphics.show_fps_overlay",     activeSettings_.showFpsOverlay);
    appPreferences_.SetInt  ("graphics.fps_overlay_corner",   activeSettings_.fpsOverlayCorner);
    appPreferences_.SetString("appearance.theme",             activeSettings_.darkTheme ? "dark" : "light");
    appPreferences_.SetInt  ("appearance.ui_scale_percent",   activeSettings_.uiScalePercent);
    appPreferences_.SetInt  ("editor.autosave_delay_ms",      activeSettings_.autosaveDelayMs);
    appPreferences_.SetBool ("welcome.show_on_startup",       activeSettings_.showWelcomeOnStartup);
    appPreferences_.SetBool ("showcase.load_on_startup",      activeSettings_.loadShowcaseOnStartup);
    appPreferences_.SetBool ("network.enabled",               activeSettings_.networkEnabled);
    (void)appPreferences_.Save();
}

void EditorUI::ApplyAppSettings(const AppSettings& settings, bool dispatchCallback) {
    const AppSettings previous = activeSettings_;
    activeSettings_ = settings;

    // Theme
    const UiTheme desiredTheme = settings.darkTheme ? UiTheme::Dark : UiTheme::Light;
    if (desiredTheme != currentTheme_) {
        currentTheme_ = desiredTheme;
        themeApplyPending_ = true;
    }

    // UI scale
    const float desiredScale = static_cast<float>(settings.uiScalePercent) / 100.0f;
    if (std::abs(desiredScale - currentDpiScale_) >= 0.001f) {
        pendingDpiScale_ = desiredScale;
    }

    // Startup mirrors — keep the legacy bools and prefs in sync so welcome/showcase popups behave.
    if (settings.showWelcomeOnStartup != showWelcomeOnStartup_) {
        showWelcomeOnStartup_ = settings.showWelcomeOnStartup;
        SaveWelcomePreference();
    }
    if (settings.loadShowcaseOnStartup != loadShowcaseWorkspaceOnStartup_) {
        loadShowcaseWorkspaceOnStartup_ = settings.loadShowcaseOnStartup;
        SaveShowcasePreference();
    }

    // Network — only fire the existing callback when it actually changed.
    if (settings.networkEnabled != networkEnabled_) {
        networkEnabled_ = settings.networkEnabled;
        SaveNetworkPreference();
        if (onNetworkEnabledChanged_) {
            onNetworkEnabledChanged_(networkEnabled_);
        }
    }

    if (dispatchCallback && onAppSettingsApplied_) {
        onAppSettingsApplied_(activeSettings_);
    }
    (void)previous;
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings window UI
// ─────────────────────────────────────────────────────────────────────────────

namespace {
    struct SettingsCategoryDef {
        const char* label;
        const char* description;
    };

    constexpr SettingsCategoryDef kSettingsCategories[] = {
        { "Graphics",   "V-sync, frame pacing, overlays." },
        { "Appearance", "Theme, UI scale." },
        { "Editor",     "Editing behavior." },
        { "Startup",    "Workspace and welcome behavior." },
        { "Network",    "LAN workspace sharing." },
    };
}

void EditorUI::DrawSettingsWindow() {
    if (!showSettingsWindow_) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (settingsWindowJustOpened_) {
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        const ImVec2 desired(820.0f * currentDpiScale_, 540.0f * currentDpiScale_);
        ImGui::SetNextWindowSize(desired, ImGuiCond_Always);
        settingsWindowJustOpened_ = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(680.0f, 420.0f),
                                        ImVec2(viewport->WorkSize.x * 0.95f,
                                               viewport->WorkSize.y * 0.95f));

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (!ImGui::Begin("Settings", &open, flags)) {
        ImGui::End();
        if (!open) {
            showSettingsWindow_ = false;
        }
        return;
    }

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    const float sidebarWidth = std::max(160.0f, 180.0f * currentDpiScale_);

    if (ImGui::BeginChild("SettingsSidebar", ImVec2(sidebarWidth, -footerHeight), true)) {
        for (int i = 0; i < static_cast<int>(sizeof(kSettingsCategories) / sizeof(kSettingsCategories[0])); ++i) {
            const bool selected = (settingsActiveCategory_ == i);
            if (ImGui::Selectable(kSettingsCategories[i].label, selected,
                                  ImGuiSelectableFlags_None,
                                  ImVec2(0.0f, ImGui::GetFrameHeight()))) {
                settingsActiveCategory_ = i;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", kSettingsCategories[i].description);
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("SettingsContent", ImVec2(0.0f, -footerHeight), true)) {
        const int category = std::clamp(settingsActiveCategory_, 0,
                                        static_cast<int>(sizeof(kSettingsCategories) / sizeof(kSettingsCategories[0])) - 1);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 10.0f));
        ImGui::TextDisabled("%s", kSettingsCategories[category].description);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        switch (category) {
            case 0: DrawSettingsCategoryGraphics();   break;
            case 1: DrawSettingsCategoryAppearance(); break;
            case 2: DrawSettingsCategoryEditor();     break;
            case 3: DrawSettingsCategoryStartup();    break;
            case 4: DrawSettingsCategoryNetwork();    break;
            default: break;
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    ImGui::Separator();

    // Footer: dirty indicator + buttons
    const bool dirty = std::memcmp(&pendingSettings_, &activeSettings_, sizeof(AppSettings)) != 0;
    if (dirty) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "* Unsaved changes");
    } else {
        ImGui::TextDisabled("All changes saved");
    }
    ImGui::SameLine();

    const float buttonW = 96.0f;
    const float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float buttonsTotalWidth = buttonW * 3.0f + buttonSpacing * 2.0f;
    const float availableX = ImGui::GetContentRegionAvail().x;
    if (availableX > buttonsTotalWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableX - buttonsTotalWidth);
    }

    if (ImGui::Button("Cancel", ImVec2(buttonW, 0.0f))) {
        pendingSettings_ = activeSettings_;
        showSettingsWindow_ = false;
    }
    ImGui::SameLine();
    if (!dirty) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Apply", ImVec2(buttonW, 0.0f))) {
        ApplyAppSettings(pendingSettings_, /*dispatchCallback=*/true);
        SaveAllSettingsToPrefs();
        pendingSettings_ = activeSettings_;
    }
    if (!dirty) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("OK", ImVec2(buttonW, 0.0f))) {
        if (dirty) {
            ApplyAppSettings(pendingSettings_, /*dispatchCallback=*/true);
            SaveAllSettingsToPrefs();
            pendingSettings_ = activeSettings_;
        }
        showSettingsWindow_ = false;
    }

    ImGui::End();
    if (!open) {
        showSettingsWindow_ = false;
    }
}

void EditorUI::DrawSettingsCategoryGraphics() {
    ImGui::Checkbox("Enable V-Sync", &pendingSettings_.vsyncEnabled);
    ImGui::TextDisabled("When on, the frame rate is locked to the monitor's refresh rate.");
    ImGui::Spacing();

    if (pendingSettings_.vsyncEnabled) {
        ImGui::BeginDisabled();
    }
    ImGui::Text("Target Framerate");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("##targetFps", &pendingSettings_.targetFramerate, 15, 480, "%d FPS");
    ImGui::SameLine();
    if (ImGui::SmallButton("60")) pendingSettings_.targetFramerate = 60;
    ImGui::SameLine();
    if (ImGui::SmallButton("120")) pendingSettings_.targetFramerate = 120;
    ImGui::SameLine();
    if (ImGui::SmallButton("144")) pendingSettings_.targetFramerate = 144;
    ImGui::SameLine();
    if (ImGui::SmallButton("240")) pendingSettings_.targetFramerate = 240;
    ImGui::TextDisabled("Only used when V-Sync is disabled. The main loop sleeps to enforce this cap.");
    if (pendingSettings_.vsyncEnabled) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Show FPS Overlay", &pendingSettings_.showFpsOverlay);
    ImGui::TextDisabled("Displays a small overlay with current FPS and frame time.");

    if (!pendingSettings_.showFpsOverlay) {
        ImGui::BeginDisabled();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    static constexpr const char* kCornerLabels[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
    const int currentCorner = std::clamp(pendingSettings_.fpsOverlayCorner, 0, 3);
    if (ImGui::BeginCombo("##fpsCorner", kCornerLabels[currentCorner])) {
        for (int i = 0; i < 4; ++i) {
            const bool selected = (i == currentCorner);
            if (ImGui::Selectable(kCornerLabels[i], selected)) {
                pendingSettings_.fpsOverlayCorner = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (!pendingSettings_.showFpsOverlay) {
        ImGui::EndDisabled();
    }
}

void EditorUI::DrawSettingsCategoryAppearance() {
    ImGui::Text("Theme");
    int themeIndex = pendingSettings_.darkTheme ? 0 : 1;
    if (ImGui::RadioButton("Dark", themeIndex == 0)) {
        pendingSettings_.darkTheme = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Light", themeIndex == 1)) {
        pendingSettings_.darkTheme = false;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("UI Scale");
    int currentScaleIdx = 1;  // 100% by default
    for (int i = 0; i < static_cast<int>(sizeof(kAllowedUiScales) / sizeof(kAllowedUiScales[0])); ++i) {
        if (kAllowedUiScales[i] == pendingSettings_.uiScalePercent) {
            currentScaleIdx = i;
            break;
        }
    }
    char comboLabel[16];
    std::snprintf(comboLabel, sizeof(comboLabel), "%d%%", pendingSettings_.uiScalePercent);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##uiScale", comboLabel)) {
        for (int i = 0; i < static_cast<int>(sizeof(kAllowedUiScales) / sizeof(kAllowedUiScales[0])); ++i) {
            char label[16];
            std::snprintf(label, sizeof(label), "%d%%", kAllowedUiScales[i]);
            const bool selected = (i == currentScaleIdx);
            if (ImGui::Selectable(label, selected)) {
                pendingSettings_.uiScalePercent = kAllowedUiScales[i];
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Ctrl+= and Ctrl+- also adjust scale at runtime.");
}

void EditorUI::DrawSettingsCategoryEditor() {
    ImGui::Text("Autosave Delay");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("##autosaveDelay", &pendingSettings_.autosaveDelayMs, 250, 10000, "%d ms");
    ImGui::TextDisabled("Documents are saved this long after the last edit.");
}

void EditorUI::DrawSettingsCategoryStartup() {
    ImGui::Checkbox("Show welcome screen on startup", &pendingSettings_.showWelcomeOnStartup);
    ImGui::Spacing();
    ImGui::Checkbox("Load showcase workspace on first launch", &pendingSettings_.loadShowcaseOnStartup);
    ImGui::TextDisabled("Applies only when the workspace folder is empty.");
}

void EditorUI::DrawSettingsCategoryNetwork() {
    ImGui::Checkbox("Enable LAN workspace sharing", &pendingSettings_.networkEnabled);
    ImGui::TextDisabled("Discover and share workspaces with other JITGL instances on the same network.");
    ImGui::Spacing();
    if (ImGui::Button("Open Network Diagnostics...")) {
        showNetworkDiagnostics_ = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FPS overlay
// ─────────────────────────────────────────────────────────────────────────────

void EditorUI::DrawFpsOverlay() {
    if (!activeSettings_.showFpsOverlay || frameTimeSampleCount_ == 0) {
        return;
    }

    float sumMs = 0.0f;
    for (int i = 0; i < frameTimeSampleCount_; ++i) {
        sumMs += frameTimeSamplesMs_[i];
    }
    const float avgMs = sumMs / static_cast<float>(frameTimeSampleCount_);
    const float fps = (avgMs > 0.0001f) ? (1000.0f / avgMs) : 0.0f;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 8.0f * currentDpiScale_;
    const int corner = std::clamp(activeSettings_.fpsOverlayCorner, 0, 3);
    const bool right  = (corner == 1 || corner == 3);
    const bool bottom = (corner == 2 || corner == 3);
    const ImVec2 pos(viewport->WorkPos.x + (right  ? viewport->WorkSize.x - margin : margin),
                     viewport->WorkPos.y + (bottom ? viewport->WorkSize.y - margin : margin));
    const ImVec2 pivot(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##FpsOverlay", nullptr, kFlags)) {
        ImGui::Text("%.1f FPS  (%.2f ms)", fps, avgMs);
    }
    ImGui::End();
}
