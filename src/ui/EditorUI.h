#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <unordered_map>
#include "TextEditor.h"

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
    void SetWorkspaces(const std::vector<std::string>& workspaceNames, const std::string& activeWorkspace);
    void SetActiveWorkspace(const std::string& workspaceName);
    void SetWorkspaceOutputHistory(const std::string& workspaceName,
                                   std::vector<std::string> consoleHistory,
                                   std::vector<std::string> logHistory);
    void SetRendererTexture(unsigned int texture, int width, int height);
    void SetCompilationStatus(bool isCompiling, bool hasError, bool isStalled = false);
    void SetupDarkTheme();

    void AddConsoleOutput(const std::string& text);
    void AddLogOutput(const std::string& text);

private:
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

    char commandBuffer[512] = "";
    std::vector<std::string> commandHistory;

    std::function<bool(const std::string&, const std::string&)> onSaveDocument_;
    std::function<void(const std::string&, const std::string&)> onDocumentChanged_;
    std::function<void(const std::string&, const std::string&)> onActiveDocumentChanged_;
    std::function<void(const std::string&)> onCreateWorkspace_;
    std::function<void(const std::string&)> onDeleteWorkspace_;
    std::function<void(const std::string&)> onWorkspaceSwitched_;
    std::function<void(const std::string&, const std::string&, bool)> onWorkspaceLineAppended_;

    std::string activeDocumentPath_;
    unsigned int rendererTexture_ = 0;
    int rendererTextureWidth_ = 0;
    int rendererTextureHeight_ = 0;

    bool isCompiling_ = false;
    bool hasCompileError_ = false;
    bool isStalled_ = false;
    bool openCreateWorkspacePopup_ = false;
    char newWorkspaceNameBuffer_[128] = "";

    void SetupDockspace();
    void DrawMenuBar();
    void DrawConsolePane();
    void DrawTextEditorPane();

    float currentDpiScale_ = 1.0f;
    float pendingDpiScale_ = 1.0f;
    void ReloadFontAtlas(float dpiScale, bool recreateTexture);
    void ApplyPendingDpiScale();
    void SetDpiScale(float newScale);

    bool shutdown_ = false;
    bool initialized_ = false;
};
