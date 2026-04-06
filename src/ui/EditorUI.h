#pragma once

#include <mutex>
#include <vector>
#include <string>
#include <TextEditor.h>

struct GLFWwindow;

struct Document{
    std::string filename;
    std::string filepath;
    
    TextEditor editor;

    bool isOpen = true;
    bool isDirty = false;           // if user typed unsaved changes
    double lastModifiedTime = 0.0;
    std::string lastKnownText;
};

class EditorUI {
public:
    EditorUI();
    ~EditorUI();

    // Lifecycle methods called by the Engine
    void Init(GLFWwindow* window);
    void NewFrame();
    void Draw();
    void Render();
    void Shutdown();

    //scanning the filesystem
    void LoadWorkspace(const std::string& directoryPath);

    void AddConsoleOutput(const std::string& text);

private:
    GLFWwindow* window;
    std::vector<Document> openDocuments;
    std::vector<std::string> consoleLines;
    std::mutex consoleMutex;
    bool scrollToBottom = true;

    // UI Component Renderers
    void SetupDockspace();
    void DrawMenuBar();
    void DrawViewportPane();
    void DrawConsolePane();
    void DrawTextEditorPane();
};