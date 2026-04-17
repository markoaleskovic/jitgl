#include "core/WorkspaceManager.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

WorkspaceManager::WorkspaceManager(std::string directory) 
    : directory_(std::move(directory)) {}

void WorkspaceManager::Initialize() {
    if (!fs::exists(directory_)) {
        fs::create_directories(directory_);
    }

    const std::string mainPath = directory_ + "/main.cpp";
    if (!fs::exists(mainPath)) {
        std::ofstream file(mainPath);
        file << "// Available: init(), update(), renderFrame()\n"
                "// EngineContext* ctx gives you: time, deltaTime, width, height, vao, vbo\n"
                "// All OpenGL functions are available directly.\n\n"
                "extern \"C\" void init(EngineContext* ctx) {\n"
                "    // Called once after first successful JIT compile\n"
                "    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);\n"
                "}\n\n"
                "extern \"C\" void update(EngineContext* ctx) {\n"
                "    // Called every frame before renderFrame\n"
                "}\n\n"
                "extern \"C\" void renderFrame(EngineContext* ctx) {\n"
                "    glClear(GL_COLOR_BUFFER_BIT);\n"
                "}\n";
        file.close();
    }
}

std::vector<WorkspaceFile> WorkspaceManager::LoadAllFiles() const {
    std::vector<WorkspaceFile> files;
    if (!fs::exists(directory_)) return files;

    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (!entry.is_regular_file()) continue;

        WorkspaceFile wf;
        wf.filename = entry.path().filename().string();
        wf.filepath = entry.path().string();

        std::ifstream file(wf.filepath, std::ios::binary);
        wf.content = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        files.push_back(wf);
    }
    return files;
}

bool WorkspaceManager::SaveFile(const std::string& filepath, const std::string& content) const {
    std::ofstream outFile(filepath, std::ios::trunc | std::ios::binary);
    if (outFile.is_open()) {
        outFile.write(content.c_str(), content.size());
        return true;
    }
    return false;
}