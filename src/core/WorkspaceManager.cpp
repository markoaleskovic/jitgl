#include "core/WorkspaceManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace {
    constexpr const char* kSceneFilename = "scene.cpp";
    constexpr const char* kShaderFilename = "shader.glsl";
    constexpr const char* kConsoleLogFilename = "console.log";
    constexpr const char* kEngineLogFilename = "engine.log";
    constexpr const char* kDefaultWorkspaceName = "default";

    std::string DefaultSceneTemplate() {
        return R"(// Workspace C++ entry points:
// init(), update(), renderFrame(), shutdown()
//
// Injected by the host at JIT compile time:
//   JIT_WORKSPACE_VERTEX_SHADER   (const char*)
//   JIT_WORKSPACE_FRAGMENT_SHADER (const char*)
//   JIT_WORKSPACE_SHADER_HASH     (uint32_t)

extern "C" void init(EngineContext* ctx) {
    GLuint currentProgram = (GLuint)STATE_I(0);
    const uint32_t cachedShaderHash = STATE_I(3);

    if (currentProgram == 0 || cachedShaderHash != JIT_WORKSPACE_SHADER_HASH) {
        if (currentProgram != 0) {
            glDeleteProgram(currentProgram);
        }

        GLuint vs = jit_compile_shader(GL_VERTEX_SHADER, JIT_WORKSPACE_VERTEX_SHADER);
        GLuint fs = jit_compile_shader(GL_FRAGMENT_SHADER, JIT_WORKSPACE_FRAGMENT_SHADER);
        GLuint program = jit_link_program(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        STATE_I(0) = (uint32_t)program;
        STATE_I(1) = (uint32_t)glGetUniformLocation(program, "uTime");
        STATE_I(3) = JIT_WORKSPACE_SHADER_HASH;

        printf("[JIT] Shader program rebuilt (hash=%u)\n", JIT_WORKSPACE_SHADER_HASH);
    }
}

extern "C" void update(EngineContext* ctx) {
    (void)ctx;
}

extern "C" void renderFrame(EngineContext* ctx) {
    GLuint program = (GLuint)STATE_I(0);
    GLint uTime = (GLint)STATE_I(1);

    glViewport(0, 0, ctx->width, ctx->height);
    glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (program == 0) {
        return;
    }

    glUseProgram(program);
    glUniform1f(uTime, ctx->time);

    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

extern "C" void shutdown(EngineContext* ctx) {
    (void)ctx;
}
)";
    }

    std::string DefaultShaderTemplate() {
        return R"(#type vertex
#version 330 core
layout (location = 0) in vec2 aPos;
out vec2 vUv;

void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}

#type fragment
#version 330 core
in vec2 vUv;
out vec4 FragColor;

uniform float uTime;

void main() {
    vec3 base = vec3(vUv.x, vUv.y, 0.5 + 0.5 * sin(uTime));
    FragColor = vec4(base, 1.0);
}
)";
    }
}

WorkspaceManager::WorkspaceManager(std::string directory)
    : directory_(std::move(directory)) {}

void WorkspaceManager::Initialize() const {
    std::error_code ec;
    if (!fs::exists(directory_, ec)) {
        fs::create_directories(directory_, ec);
    }
    if (ec) {
        return;
    }

    if (!ListWorkspaces().empty()) {
        return;
    }

    // Fresh install path: create "default" workspace and optionally migrate legacy main.cpp.
    auto descriptor = CreateWorkspace(kDefaultWorkspaceName);
    if (!descriptor.has_value()) {
        return;
    }

    const fs::path legacyMainPath = fs::path(directory_) / "main.cpp";
    if (fs::exists(legacyMainPath, ec) && !ec) {
        auto legacyContent = ReadFile(legacyMainPath.string());
        if (legacyContent.has_value() && !legacyContent->empty()) {
            SaveFile(descriptor->cppPath, *legacyContent);
        }
    }
}

std::vector<WorkspaceDescriptor> WorkspaceManager::ListWorkspaces() const {
    std::vector<WorkspaceDescriptor> workspaces;
    std::error_code ec;
    if (!fs::exists(directory_, ec) || ec) {
        return workspaces;
    }

    for (const auto& entry : fs::directory_iterator(directory_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory(ec) || ec) {
            continue;
        }

        const std::string workspaceName = entry.path().filename().string();
        auto descriptor = BuildDescriptor(workspaceName);
        if (!descriptor.has_value()) {
            continue;
        }

        ec.clear();
        const bool hasCpp = fs::exists(descriptor->cppPath, ec);
        if (ec) {
            continue;
        }

        ec.clear();
        const bool hasShader = fs::exists(descriptor->shaderPath, ec);
        if (ec) {
            continue;
        }

        if (hasCpp && hasShader) {
            workspaces.emplace_back(*descriptor);
        }
    }

    std::ranges::sort(workspaces, [](const WorkspaceDescriptor& lhs, const WorkspaceDescriptor& rhs) {
        return lhs.name < rhs.name;
    });
    return workspaces;
}

std::optional<WorkspaceDescriptor> WorkspaceManager::GetWorkspace(const std::string& workspaceName) const {
    auto descriptor = BuildDescriptor(workspaceName);
    if (!descriptor.has_value()) {
        return std::nullopt;
    }

    std::error_code ec;
    if (!fs::exists(descriptor->cppPath, ec) || ec) {
        return std::nullopt;
    }
    if (!fs::exists(descriptor->shaderPath, ec) || ec) {
        return std::nullopt;
    }

    return descriptor;
}

std::optional<WorkspaceDescriptor> WorkspaceManager::CreateWorkspace(const std::string& workspaceName) const {
    auto descriptor = BuildDescriptor(workspaceName);
    if (!descriptor.has_value()) {
        return std::nullopt;
    }

    if (!EnsureWorkspaceScaffold(*descriptor)) {
        return std::nullopt;
    }

    return descriptor;
}

bool WorkspaceManager::DeleteWorkspace(const std::string& workspaceName) const {
    auto descriptor = BuildDescriptor(workspaceName);
    if (!descriptor.has_value()) {
        return false;
    }

    std::error_code ec;
    if (!fs::exists(descriptor->directory, ec) || ec) {
        return false;
    }

    fs::remove_all(descriptor->directory, ec);
    return !ec;
}

std::vector<std::string> WorkspaceManager::LoadWorkspaceConsoleLog(const std::string& workspaceName) const {
    auto descriptor = GetWorkspace(workspaceName);
    if (!descriptor.has_value()) {
        return {};
    }
    return LoadLogFileLines(descriptor->consoleLogPath);
}

std::vector<std::string> WorkspaceManager::LoadWorkspaceEngineLog(const std::string& workspaceName) const {
    auto descriptor = GetWorkspace(workspaceName);
    if (!descriptor.has_value()) {
        return {};
    }
    return LoadLogFileLines(descriptor->engineLogPath);
}

bool WorkspaceManager::AppendWorkspaceConsoleLog(const std::string& workspaceName, const std::string& line) const {
    auto descriptor = GetWorkspace(workspaceName);
    if (!descriptor.has_value()) {
        return false;
    }
    return AppendLogFileLine(descriptor->consoleLogPath, line);
}

bool WorkspaceManager::AppendWorkspaceEngineLog(const std::string& workspaceName, const std::string& line) const {
    auto descriptor = GetWorkspace(workspaceName);
    if (!descriptor.has_value()) {
        return false;
    }
    return AppendLogFileLine(descriptor->engineLogPath, line);
}

std::vector<WorkspaceFile> WorkspaceManager::LoadAllFiles() const {
    std::vector<WorkspaceFile> files;
    for (const auto& workspace : ListWorkspaces()) {
        auto cppContent = ReadFile(workspace.cppPath);
        auto shaderContent = ReadFile(workspace.shaderPath);
        if (cppContent.has_value()) {
            files.emplace_back(WorkspaceFile{
                workspace.name + "/" + kSceneFilename,
                workspace.cppPath,
                std::move(*cppContent)
            });
        }
        if (shaderContent.has_value()) {
            files.emplace_back(WorkspaceFile{
                workspace.name + "/" + kShaderFilename,
                workspace.shaderPath,
                std::move(*shaderContent)
            });
        }
    }

    std::ranges::sort(files, [](const WorkspaceFile& lhs, const WorkspaceFile& rhs) {
        return lhs.filename < rhs.filename;
    });
    return files;
}

bool WorkspaceManager::SaveFile(const std::string& filepath, const std::string& content) const {
    if (!IsPathInsideWorkspace(filepath)) {
        return false;
    }

    std::ofstream outFile(filepath, std::ios::trunc | std::ios::binary);
    if (!outFile.is_open()) {
        return false;
    }

    outFile.write(content.c_str(), static_cast<std::streamsize>(content.size()));
    return outFile.good();
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

bool WorkspaceManager::IsPathInsideWorkspace(const fs::path& filepath) const {
    std::error_code ec;
    const fs::path workspacePath = fs::weakly_canonical(fs::path(directory_), ec);
    if (ec) {
        return false;
    }

    ec.clear();
    fs::path targetPath = fs::weakly_canonical(filepath, ec);
    if (ec) {
        targetPath = fs::absolute(filepath, ec);
    }
    if (ec) {
        return false;
    }

    // Compare normalized string prefixes so "../" traversal cannot escape workspace root.
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

std::optional<WorkspaceDescriptor> WorkspaceManager::BuildDescriptor(const std::string& workspaceName) const {
    if (!IsValidWorkspaceName(workspaceName)) {
        return std::nullopt;
    }

    const fs::path workspaceDir = fs::path(directory_) / workspaceName;
    WorkspaceDescriptor descriptor;
    descriptor.name = workspaceName;
    descriptor.directory = workspaceDir.string();
    descriptor.cppPath = (workspaceDir / kSceneFilename).string();
    descriptor.shaderPath = (workspaceDir / kShaderFilename).string();
    descriptor.consoleLogPath = (workspaceDir / kConsoleLogFilename).string();
    descriptor.engineLogPath = (workspaceDir / kEngineLogFilename).string();
    return descriptor;
}

bool WorkspaceManager::EnsureWorkspaceScaffold(const WorkspaceDescriptor& descriptor) const {
    std::error_code ec;
    fs::create_directories(descriptor.directory, ec);
    if (ec) {
        return false;
    }

    if (!fs::exists(descriptor.cppPath, ec) || ec) {
        std::ofstream sceneFile(descriptor.cppPath, std::ios::binary);
        if (!sceneFile.is_open()) {
            return false;
        }
        sceneFile << DefaultSceneTemplate();
    }

    ec.clear();
    if (!fs::exists(descriptor.shaderPath, ec) || ec) {
        std::ofstream shaderFile(descriptor.shaderPath, std::ios::binary);
        if (!shaderFile.is_open()) {
            return false;
        }
        shaderFile << DefaultShaderTemplate();
    }

    ec.clear();
    if (!fs::exists(descriptor.consoleLogPath, ec) || ec) {
        std::ofstream consoleLog(descriptor.consoleLogPath, std::ios::app | std::ios::binary);
        if (!consoleLog.is_open()) {
            return false;
        }
    }

    ec.clear();
    if (!fs::exists(descriptor.engineLogPath, ec) || ec) {
        std::ofstream engineLog(descriptor.engineLogPath, std::ios::app | std::ios::binary);
        if (!engineLog.is_open()) {
            return false;
        }
    }

    return true;
}

bool WorkspaceManager::IsValidWorkspaceName(const std::string& workspaceName) {
    if (workspaceName.empty()) {
        return false;
    }
    for (const char c : workspaceName) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            continue;
        }
        if (c == '_' || c == '-') {
            continue;
        }
        return false;
    }
    return true;
}

std::vector<std::string> WorkspaceManager::LoadLogFileLines(const std::string& logPath) {
    std::vector<std::string> lines;
    std::ifstream in(logPath, std::ios::binary);
    if (!in.is_open()) {
        return lines;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    return lines;
}

bool WorkspaceManager::AppendLogFileLine(const std::string& logPath, const std::string& line) {
    std::ofstream out(logPath, std::ios::app | std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out << line << '\n';
    return out.good();
}
