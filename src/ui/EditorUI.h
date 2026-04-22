#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "TextEditor.h"

struct GLFWwindow;

struct Document {
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
    void AddDocument(const std::string& filename, const std::string& filepath, const std::string& content);
    void UpdateDocumentContent(const std::string& filepath, const std::string& content);
    void SetSaveCallback(std::function<bool(const std::string&, const std::string&)> cb);
    void SetDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb);
    void SetActiveDocumentChangedCallback(std::function<void(const std::string&, const std::string&)> cb);
    void SetRendererTexture(unsigned int texture, int width, int height);

    void AddConsoleOutput(const std::string& text);
    void AddLogOutput(const std::string& text);

private:
    GLFWwindow* window;
    std::vector<Document> openDocuments;
    std::vector<std::string> consoleLines;
    std::vector<std::string> logLines;
    std::mutex consoleMutex;
    std::mutex logMutex;

    bool consoleScrollToBottom = true;
    bool logScrollToBottom = true;

    char commandBuffer[512] = "";
    std::vector<std::string> commandHistory;

    std::function<bool(const std::string&, const std::string&)> onSaveDocument_;
    std::function<void(const std::string&, const std::string&)> onDocumentChanged_;
    std::function<void(const std::string&, const std::string&)> onActiveDocumentChanged_;

    std::string activeDocumentPath_;
    unsigned int rendererTexture_ = 0;
    int rendererTextureWidth_ = 0;
    int rendererTextureHeight_ = 0;

    void SetupDockspace();
    void DrawMenuBar();
    void DrawConsolePane();
    void DrawTextEditorPane();

    bool shutdown_ = false;
    bool initialized_ = false;
};
