#include "core/WorkspaceManager.h"
#include <filesystem>
#include <fstream>
#include <algorithm>

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

        auto content = ReadFile(wf.filepath);
        if (!content.has_value()) {
            continue;
        }
        wf.content = std::move(*content);
        files.push_back(wf);
    }

    std::sort(files.begin(), files.end(), [](const WorkspaceFile& lhs, const WorkspaceFile& rhs) {
        return lhs.filename < rhs.filename;
    });
    return files;
}

bool WorkspaceManager::SaveFile(const std::string& filepath, const std::string& content) const {
    if (!IsPathInsideWorkspace(filepath)) {
        return false;
    }

    std::ofstream outFile(filepath, std::ios::trunc | std::ios::binary);
    if (outFile.is_open()) {
        outFile.write(content.c_str(), content.size());
        return outFile.good();
    }
    return false;
}

std::optional<std::string> WorkspaceManager::ReadFile(const std::string& filepath) const {
    if (!IsPathInsideWorkspace(filepath)) {
        return std::nullopt;
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool WorkspaceManager::IsPathInsideWorkspace(const std::string& filepath) const {
    std::error_code ec;
    const fs::path workspacePath = fs::weakly_canonical(fs::path(directory_), ec);
    if (ec) {
        return false;
    }

    ec.clear();
    fs::path targetPath = fs::weakly_canonical(fs::path(filepath), ec);
    if (ec) {
        targetPath = fs::absolute(fs::path(filepath), ec);
    }
    if (ec) {
        return false;
    }

    const auto workspaceStr = workspacePath.lexically_normal().string();
    const auto targetStr = targetPath.lexically_normal().string();

    if (targetStr.size() < workspaceStr.size()) {
        return false;
    }
    if (targetStr.compare(0, workspaceStr.size(), workspaceStr) != 0) {
        return false;
    }

    if (targetStr.size() == workspaceStr.size()) {
        return true;
    }

    const char separator = fs::path::preferred_separator;
    return targetStr[workspaceStr.size()] == separator;
}
