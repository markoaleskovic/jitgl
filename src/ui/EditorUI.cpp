#include "ui/EditorUI.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <string>
#include <imgui_internal.h>
#include <algorithm>
#include <cstddef>

namespace {
    constexpr double AUTOSAVE_DEBOUNCE_SECONDS = 0.05;
    constexpr const char* GLSL_VERSION = "#version 330 core";
    constexpr std::size_t MAX_CONSOLE_LINES = 2000;
    constexpr std::size_t MAX_LOG_LINES = 2000;
}

EditorUI::EditorUI() : window(nullptr) {}

EditorUI::~EditorUI() {
    Shutdown();
}

void EditorUI::Init(GLFWwindow *win) {
    window = win;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);

    initialized_ = true;
    shutdown_ = false;

}

void EditorUI::NewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EditorUI::Draw() {
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

void EditorUI::AddDocument(const std::string& filename, const std::string& filepath, const std::string& content) {
    auto existing = std::find_if(openDocuments.begin(), openDocuments.end(), [&](const Document& doc) {
        return doc.filepath == filepath;
    });
    if (existing != openDocuments.end()) {
        existing->filename = filename;
        existing->editor.SetText(content);
        existing->lastKnownText = content;
        existing->isDirty = false;
        return;
    }

    Document doc;
    doc.filename = filename;
    doc.filepath = filepath;
    doc.editor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    doc.editor.SetText(content);
    doc.lastKnownText = content;
    doc.isDirty = false;
    doc.isOpen = true;
    openDocuments.push_back(doc);

    if (activeDocumentPath_.empty()) {
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

void EditorUI::SetSaveCallback(std::function<bool(const std::string&, const std::string&)> cb) {
    onSaveDocument_ = std::move(cb);
}

void EditorUI::SetDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    onDocumentChanged_ = std::move(cb);
}

void EditorUI::SetActiveDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    onActiveDocumentChanged_ = std::move(cb);
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

void EditorUI::DrawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New")) {}
            if (ImGui::MenuItem("Save", "Ctrl+S")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("View")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Help")) { ImGui::EndMenu(); }

        // Compile Status Indicator
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 150.0f);
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

    if (ImGui::BeginTabBar("EditorTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        double currentTime = ImGui::GetTime();

        for (auto &doc: openDocuments) {
            if (!doc.isOpen) continue;

            ImGuiTabItemFlags tabFlags = doc.isDirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;

            if (ImGui::BeginTabItem(doc.filename.c_str(), &doc.isOpen, tabFlags)) {
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

            if (doc.isDirty && (currentTime - doc.lastModifiedTime) >= AUTOSAVE_DEBOUNCE_SECONDS) {
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
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void EditorUI::AddConsoleOutput(const std::string &text) {
    std::lock_guard<std::mutex> lock(consoleMutex);
    consoleLines.push_back(text);
    if (consoleLines.size() > MAX_CONSOLE_LINES) {
        const std::size_t removeCount = consoleLines.size() - MAX_CONSOLE_LINES;
        consoleLines.erase(consoleLines.begin(), consoleLines.begin() + static_cast<std::ptrdiff_t>(removeCount));
    }
    consoleScrollToBottom = true;
}

void EditorUI::AddLogOutput(const std::string &text) {
    std::lock_guard<std::mutex> lock(logMutex);
    logLines.push_back(text);
    if (logLines.size() > MAX_LOG_LINES) {
        const std::size_t removeCount = logLines.size() - MAX_LOG_LINES;
        logLines.erase(logLines.begin(), logLines.begin() + static_cast<std::ptrdiff_t>(removeCount));
    }
    logScrollToBottom = true;
}

void EditorUI::DrawConsolePane() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("Utility View", nullptr, flags);

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
                for (const std::string &line: consoleLines) {
                    if (!line.empty() && line[0] == '>') {
                        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", line.c_str());
                    } else {
                        ImGui::TextUnformatted(line.c_str());
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
                for (const std::string &line: logLines) {
                    if (line.find("Error") != std::string::npos || line.find("Failed") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", line.c_str());
                    } else if (line.find("[AutoSave]") != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", line.c_str());
                    } else {
                        ImGui::TextUnformatted(line.c_str());
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