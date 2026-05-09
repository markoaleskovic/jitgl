#include "core/Engine.h"
#include "ui/EditorUI.h"
#include "watcher/FileWatcher.h"
#include "system/ConsoleRedirectSession.h"
#include "core/WorkspaceManager.h"
#include "runtime/EngineContext.h"
#include "jit/JitEngine.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <ranges>
#include <sstream>
#include <utility>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

namespace {
#ifndef JIT_PROJECT_BINARY_DIR
#define JIT_PROJECT_BINARY_DIR "."
#endif

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr const char* WINDOW_TITLE = "JITGL";

    constexpr int GL_VERSION_MAJOR = 3;
    constexpr int GL_VERSION_MINOR = 3;

    constexpr const char* WORKSPACE_DIR = JIT_PROJECT_BINARY_DIR "/workspace";
    constexpr const char* PREAMBLE_PATH = JIT_PROJECT_BINARY_DIR "/runtime/engine.hpp";

    constexpr double MIN_DEBOUNCE_SECONDS = 0.15;
    constexpr double MAX_DEBOUNCE_SECONDS = 1.95;
    constexpr double DEBOUNCE_STEP_SECONDS = 0.2;
    constexpr double WATCHER_IGNORE_SECONDS = 1.0;
    constexpr double ERROR_HISTORY_SECONDS = 5.0;

    std::string trim(std::string value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    bool parseShaderSections(const std::string& shaderSource,
                             std::string* vertexShaderOut,
                             std::string* fragmentShaderOut) {
        if (!vertexShaderOut || !fragmentShaderOut) {
            return false;
        }
        vertexShaderOut->clear();
        fragmentShaderOut->clear();

        enum class Section {
            None,
            Vertex,
            Fragment,
        };

        Section currentSection = Section::None;
        std::size_t cursor = 0;

        while (cursor <= shaderSource.size()) {
            const std::size_t lineEnd = shaderSource.find('\n', cursor);
            const std::size_t lineLength = (lineEnd == std::string::npos) ? (shaderSource.size() - cursor)
                                                                           : (lineEnd - cursor);
            const std::string line = shaderSource.substr(cursor, lineLength);
            if (const std::string trimmedLine = trim(line); trimmedLine.rfind("#type", 0) == 0) {
                if (trimmedLine.find("vertex") != std::string::npos) {
                    currentSection = Section::Vertex;
                } else if (trimmedLine.find("fragment") != std::string::npos) {
                    currentSection = Section::Fragment;
                } else {
                    currentSection = Section::None;
                }
            } else {
                if (currentSection == Section::Vertex) {
                    *vertexShaderOut += line;
                    *vertexShaderOut += '\n';
                } else if (currentSection == Section::Fragment) {
                    *fragmentShaderOut += line;
                    *fragmentShaderOut += '\n';
                }
            }

            if (lineEnd == std::string::npos) {
                break;
            }
            cursor = lineEnd + 1;
        }

        return !vertexShaderOut->empty() && !fragmentShaderOut->empty();
    }

    std::string QuoteForPosixShell(const std::string& value) {
        std::string quoted = "'";
        for (const char c : value) {
            if (c == '\'') {
                quoted += "'\"'\"'";
            } else {
                quoted.push_back(c);
            }
        }
        quoted += "'";
        return quoted;
    }

    std::string EscapeForAppleScriptDoubleQuoted(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size() * 2);
        for (const char c : value) {
            if (c == '\\' || c == '"') {
                escaped.push_back('\\');
            }
            escaped.push_back(c);
        }
        return escaped;
    }

    std::string EscapeForPowerShellSingleQuoted(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size() * 2);
        for (const char c : value) {
            escaped.push_back(c);
            if (c == '\'') {
                escaped.push_back('\'');
            }
        }
        return escaped;
    }

    std::string CurrentExecutablePath() {
#if defined(_WIN32)
        std::array<char, MAX_PATH> buffer{};
        const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
        return std::string(buffer.data(), length);
#elif defined(__APPLE__)
        uint32_t size = 0;
        (void)_NSGetExecutablePath(nullptr, &size);
        if (size == 0) {
            return {};
        }
        std::vector<char> raw(size + 1, '\0');
        if (_NSGetExecutablePath(raw.data(), &size) != 0) {
            return {};
        }
        std::array<char, PATH_MAX> resolved{};
        if (realpath(raw.data(), resolved.data()) != nullptr) {
            return std::string(resolved.data());
        }
        return std::string(raw.data());
#else
        std::error_code ec;
        const auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) {
            return {};
        }
        return exePath.string();
#endif
    }

#if defined(__linux__) || defined(__APPLE__)
    int NormalizeSystemExitCode(int status) {
        if (status == -1) {
            return -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return status;
    }
#endif

    int RunShellCommand(const std::string& command) {
        const int status = std::system(command.c_str());
#if defined(__linux__) || defined(__APPLE__)
        return NormalizeSystemExitCode(status);
#else
        return status;
#endif
    }

    std::string escapeForCStringLiteral(const std::string& text) {
        std::string escaped;
        escaped.reserve(text.size() * 2);
        for (const char ch : text) {
            switch (ch) {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': break;
                case '\t': escaped += "\\t"; break;
                default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }
}

Engine::Engine() = default;
Engine::~Engine() { Shutdown(); }

bool Engine::Init() {
    if (!InitWindow()) return false;
    if (!InitGL()) return false;
    if (!InitUI()) return false;
    if (!InitJIT()) return false;
    if (!InitWatcher()) return false;
    if (!InitLanShare()) {
        ui_->AddLogOutput("[Network] LAN workspace sharing is unavailable.");
    }

    lastTime_ = glfwGetTime();
    ui_->AddLogOutput("[Engine] Initialized successfully.");
    return true;
}

bool Engine::InitWindow() {
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_VERSION_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_VERSION_MINOR);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, WINDOW_TITLE, nullptr, nullptr);
    if (!window_) {
        std::cerr << "Window creation failed\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    return true;
}

bool Engine::EnsureSceneRenderTarget(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (sceneFbo_ != 0 && sceneWidth_ == width && sceneHeight_ == height) {
        return true;
    }

    DestroySceneRenderTarget();

    glGenFramebuffers(1, &sceneFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);

    glGenTextures(1, &sceneColorTex_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColorTex_, 0);

    glGenRenderbuffers(1, &sceneDepthRbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepthRbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneDepthRbo_);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (ui_) {
            ui_->AddLogOutput("[Renderer Error] Failed to initialize scene framebuffer.");
        } else {
            std::cerr << "[Renderer Error] Failed to initialize scene framebuffer.\n";
        }
        DestroySceneRenderTarget();
        return false;
    }

    sceneWidth_ = width;
    sceneHeight_ = height;
    if (ui_) {
        ui_->SetRendererTexture(sceneColorTex_, sceneWidth_, sceneHeight_);
    }
    return true;
}

void Engine::DestroySceneRenderTarget() {
    if (sceneDepthRbo_ != 0) {
        glDeleteRenderbuffers(1, &sceneDepthRbo_);
        sceneDepthRbo_ = 0;
    }
    if (sceneColorTex_ != 0) {
        glDeleteTextures(1, &sceneColorTex_);
        sceneColorTex_ = 0;
    }
    if (sceneFbo_ != 0) {
        glDeleteFramebuffers(1, &sceneFbo_);
        sceneFbo_ = 0;
    }
    sceneWidth_ = 0;
    sceneHeight_ = 0;
    if (ui_) {
        ui_->SetRendererTexture(0, 0, 0);
    }
}

unsigned int Engine::CreateShaderProgram(const char* vsSource, const char* fsSource) {
    auto compile = [](unsigned int type, const char* source) {
        unsigned int s = glCreateShader(type);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        int success;
        glGetShaderiv(s, GL_COMPILE_STATUS, &success);
        if (!success) {
            std::array<char, 512> info{};
            glGetShaderInfoLog(s, static_cast<GLsizei>(info.size()), nullptr, info.data());
            std::cerr << "Shader compile error: " << info.data() << "\n";
        }
        return s;
    };

    unsigned int vs = compile(GL_VERTEX_SHADER, vsSource);
    unsigned int fs = compile(GL_FRAGMENT_SHADER, fsSource);
    unsigned int p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    int success;
    glGetProgramiv(p, GL_LINK_STATUS, &success);
    if (!success) {
        std::array<char, 512> info{};
        glGetProgramInfoLog(p, static_cast<GLsizei>(info.size()), nullptr, info.data());
        std::cerr << "Program link error: " << info.data() << "\n";
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

bool Engine::InitGL() {
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::cerr << "GLAD init failed\n";
        return false;
    }

    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    ctx_.width = w;
    ctx_.height = h;

    const std::array<float, 12> quadVerts = {
        -1.f, -1.f, 1.f, -1.f, 1.f, 1.f,
        -1.f, -1.f, 1.f, 1.f, -1.f, 1.f,
    };
    glGenVertexArrays(1, &ctx_.vao);
    glGenBuffers(1, &ctx_.vbo);
    glBindVertexArray(ctx_.vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx_.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(quadVerts)), quadVerts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

    // Provide a default shader so the user has something working out-of-the-box
    const char* vs = R"(
        #version 330 core
        layout (location = 0) in vec2 aPos;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )";
    const char* fs = R"(
        #version 330 core
        out vec4 FragColor;
        uniform float time;
        void main() {
            FragColor = vec4(0.5 + 0.5*cos(time), 0.5 + 0.5*sin(time), 0.5, 1.0);
        }
    )";
    ctx_.defaultShader = CreateShaderProgram(vs, fs);

    return EnsureSceneRenderTarget(w, h);
}

bool Engine::InitUI() {
    ui_ = std::make_unique<EditorUI>();
    ui_->Init(window_);
    if (sceneColorTex_ != 0 && sceneWidth_ > 0 && sceneHeight_ > 0) {
        ui_->SetRendererTexture(sceneColorTex_, sceneWidth_, sceneHeight_);
    }

    consoleRedirect_ = std::make_unique<ConsoleRedirectSession>();
    if (!consoleRedirect_->Start(ui_.get())) {
        std::cerr << "Console redirect init failed\n";
        return false;
    }

    ui_->SetSaveCallback([this](const std::string& path, const std::string& content) {
        return workspaceManager_->SaveFile(path, content);
    });
    ui_->SetDocumentChangedCallback([this](const std::string& path, const std::string& content) {
        HandleDocumentEdited(path, content);
    });
    ui_->SetActiveDocumentChangedCallback([this](const std::string& path, const std::string& content) {
        HandleActiveDocumentChanged(path, content);
    });
    ui_->SetCreateWorkspaceCallback([this](const std::string& name) {
        CreateWorkspaceFromUI(name);
    });
    ui_->SetDeleteWorkspaceCallback([this](const std::string& name) {
        DeleteWorkspaceFromUI(name);
    });
    ui_->SetWorkspaceSwitchedCallback([this](const std::string& name) {
        SwitchToWorkspace(name, false);
    });
    ui_->SetExportWorkspaceCallback([this](const std::string& path) {
        return ExportActiveWorkspace(path);
    });
    ui_->SetImportWorkspaceCallback([this](const std::string& path) {
        return ImportWorkspace(path);
    });
    ui_->SetShareWorkspaceCallback([this](const std::vector<std::string>& targetPeerIds, bool shareToAll) {
        HandleShareWorkspaceRequest(targetPeerIds, shareToAll);
    });
    ui_->SetWorkspaceShareDecisionCallback([this](const std::string& offerId, bool accepted) {
        HandleWorkspaceShareDecision(offerId, accepted);
    });
    ui_->SetRequestFirewallAccessCallback([this]() {
        HandleRequestFirewallAccess();
    });

    return true;
}

bool Engine::InitJIT() {
    auto nextJit = std::make_shared<JitEngine>();
    nextJit->SetOutputCallback([this](const std::string& msg) {
        ui_->AddLogOutput(msg);
    });

    if (!nextJit->Init(PREAMBLE_PATH)) {
        ui_->AddLogOutput("[Engine] JIT engine failed to initialize.");
        return false;
    }

    jit_ = std::move(nextJit);
    compileThreadRunning_ = std::make_shared<std::atomic<bool>>(true);
    compileThreadExited_ = std::make_shared<std::atomic<bool>>(false);
    compileThread_ = std::jthread([this, running = compileThreadRunning_, exited = compileThreadExited_](std::stop_token) {
        CompileThreadMain(running);
        exited->store(true);
    });

    return true;
}

void Engine::ResetJIT() {
    if (resetRequested_) {
        return;
    }

    resetRequested_ = true;
    resetWaitLogged_ = false;
    ui_->AddLogOutput("[JIT] Compilation stalled for too long. Restart requested.");

    if (compileThreadRunning_) {
        // Let the worker exit gracefully; CompletePendingJITReset() handles restart on main thread.
        compileThreadRunning_->store(false);
    }
    compileCv_.notify_all();
}

void Engine::CompletePendingJITReset() {
    if (!resetRequested_) {
        return;
    }

    if (!compileThreadExited_ || !compileThreadExited_->load()) {
        if (!resetWaitLogged_) {
            ui_->AddLogOutput("[JIT] Waiting for compile thread to finish before restart.");
            resetWaitLogged_ = true;
        }
        ui_->SetCompilationStatus(true, ActiveWorkspaceHasCompileError(), true);
        return;
    }

    if (compileThread_.joinable()) {
        compileThread_.join();
    }

    {
        std::scoped_lock lock(compileMutex_);
        compileJobs_.clear();
        compileResults_ = std::queue<CompileResult>{};
        inFlightSourceHashes_.clear();
        inFlightStartTime_ = 0.0;
    }

    resetRequested_ = false;
    resetWaitLogged_ = false;

    if (!InitJIT()) {
        ui_->AddLogOutput("[Engine] Failed to restart JIT engine after stall.");
        return;
    }

    ui_->AddLogOutput("[JIT] JIT engine restarted.");
}

std::string Engine::WorkspaceForPath(const std::string& filepath) const {
    if (const auto it = fileToWorkspace_.find(filepath); it != fileToWorkspace_.end()) {
        return it->second;
    }
    return {};
}

bool Engine::ActiveWorkspaceHasCompileError() const {
    if (activeWorkspaceName_.empty()) {
        return false;
    }
    return compileFailures_.contains(activeWorkspaceName_);
}

void Engine::SaveActiveWorkspaceRuntimeState() {
    if (activeWorkspaceName_.empty()) {
        return;
    }

    auto workspaceIt = workspaces_.find(activeWorkspaceName_);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    auto& workspace = workspaceIt->second;
    // Snapshot persisted runtime state before switching programs/workspaces.
    std::ranges::copy(ctx_.state_i, workspace.stateI.begin());
    std::ranges::copy(ctx_.state_f, workspace.stateF.begin());
    workspace.userData = ctx_.userData;
}

void Engine::LoadWorkspaceRuntimeState(const std::string& workspaceName) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    const auto& workspace = workspaceIt->second;
    std::ranges::copy(workspace.stateI, std::begin(ctx_.state_i));
    std::ranges::copy(workspace.stateF, std::begin(ctx_.state_f));
    ctx_.userData = workspace.userData;
}

void Engine::UpdateWorkspaceSourceFromDocument(const std::string& workspaceName,
                                               const std::string& filepath,
                                               const std::string& content) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    auto& workspace = workspaceIt->second;
    bool changed = false;
    if (filepath == workspace.cppPath) {
        changed = (workspace.cppSource != content);
        workspace.cppSource = content;
    } else if (filepath == workspace.shaderPath) {
        changed = (workspace.shaderSource != content);
        workspace.shaderSource = content;
    }

    if (changed) {
        workspaceDirty_[workspaceName] = true;
    }
}

std::string Engine::BuildCompileSourceForWorkspace(const std::string& workspaceName) const {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return {};
    }

    const auto& workspace = workspaceIt->second;
    std::string vertexShader;
    std::string fragmentShader;

    std::ostringstream generatedSource;
    if (!parseShaderSections(workspace.shaderSource, &vertexShader, &fragmentShader)) {
        generatedSource << "#error \"shader.glsl must define '#type vertex' and '#type fragment' sections\"\n";
        generatedSource << workspace.cppSource;
        return generatedSource.str();
    }

    // Inject parsed shader sources and hash into the generated C++ translation unit.
    const uint32_t shaderHash = static_cast<uint32_t>(std::hash<std::string>{}(workspace.shaderSource));
    generatedSource << "static const unsigned int jitgl_workspace_shader_hash = " << shaderHash << "u;\n";
    generatedSource << "static const char* jitgl_workspace_vertex_shader_source = \""
                    << escapeForCStringLiteral(vertexShader) << "\";\n";
    generatedSource << "static const char* jitgl_workspace_fragment_shader_source = \""
                    << escapeForCStringLiteral(fragmentShader) << "\";\n";
    generatedSource << "#define JIT_WORKSPACE_VERTEX_SHADER jitgl_workspace_vertex_shader_source\n";
    generatedSource << "#define JIT_WORKSPACE_FRAGMENT_SHADER jitgl_workspace_fragment_shader_source\n";
    generatedSource << "#define JIT_WORKSPACE_SHADER_HASH jitgl_workspace_shader_hash\n";
    generatedSource << workspace.cppSource;

    return generatedSource.str();
}

void Engine::QueueCompileForWorkspace(const std::string& workspaceName, double nowSeconds, bool immediate) {
    const std::string generatedSource = BuildCompileSourceForWorkspace(workspaceName);
    if (generatedSource.empty()) {
        return;
    }
    QueueCompile(workspaceName, generatedSource, nowSeconds, immediate);
}

bool Engine::RegisterWorkspace(const WorkspaceDescriptor& descriptor) {
    if (!workspaceManager_) {
        return false;
    }

    auto cppSource = workspaceManager_->ReadFile(descriptor.cppPath);
    auto shaderSource = workspaceManager_->ReadFile(descriptor.shaderPath);
    if (!cppSource.has_value() || !shaderSource.has_value()) {
        return false;
    }

    WorkspaceState workspace;
    workspace.name = descriptor.name;
    workspace.directory = descriptor.directory;
    workspace.cppPath = descriptor.cppPath;
    workspace.shaderPath = descriptor.shaderPath;
    workspace.consoleLogPath = descriptor.consoleLogPath;
    workspace.engineLogPath = descriptor.engineLogPath;
    workspace.cppSource = std::move(*cppSource);
    workspace.shaderSource = std::move(*shaderSource);

    workspaces_[workspace.name] = workspace;
    fileToWorkspace_[workspace.cppPath] = workspace.name;
    fileToWorkspace_[workspace.shaderPath] = workspace.name;
    workspaceDirty_[workspace.name] = false;

    if (std::ranges::find(workspaceOrder_, workspace.name) == workspaceOrder_.end()) {
        workspaceOrder_.emplace_back(workspace.name);
        std::ranges::sort(workspaceOrder_);
    }

    ui_->AddDocument(workspace.name, "scene.cpp", workspace.cppPath, workspace.cppSource);
    ui_->AddDocument(workspace.name, "shader.glsl", workspace.shaderPath, workspace.shaderSource);
    ui_->SetWorkspaceOutputHistory(workspace.name,
                                   workspaceManager_->LoadWorkspaceConsoleLog(workspace.name),
                                   workspaceManager_->LoadWorkspaceEngineLog(workspace.name));
    return true;
}

void Engine::SyncWorkspaceUiState() {
    if (!ui_) {
        return;
    }
    ui_->SetWorkspaces(workspaceOrder_, activeWorkspaceName_);
}

bool Engine::CreateWorkspaceFromUI(const std::string& workspaceName) {
    if (!workspaceManager_) {
        return false;
    }

    auto descriptor = workspaceManager_->CreateWorkspace(workspaceName);
    if (!descriptor.has_value()) {
        ui_->AddLogOutput("[Workspace Error] Failed to create workspace '" + workspaceName +
                          "'. Use letters, numbers, '_' or '-'.");
        return false;
    }

    if (!RegisterWorkspace(*descriptor)) {
        ui_->AddLogOutput("[Workspace Error] Workspace created but failed to load '" + workspaceName + "'.");
        return false;
    }

    SyncWorkspaceUiState();
    SwitchToWorkspace(descriptor->name, true);
    ui_->AddLogOutput("[Workspace] Created workspace '" + descriptor->name + "'.");
    return true;
}

bool Engine::DeleteWorkspaceFromUI(const std::string& workspaceName) {
    if (!workspaceManager_) {
        return false;
    }
    if (workspaceOrder_.size() <= 1) {
        ui_->AddLogOutput("[Workspace Error] Cannot delete the last workspace.");
        return false;
    }

    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        ui_->AddLogOutput("[Workspace Error] Workspace '" + workspaceName + "' is not loaded.");
        return false;
    }

    const WorkspaceState workspace = workspaceIt->second;
    const bool deletingActiveWorkspace = (workspaceName == activeWorkspaceName_);

    std::string nextWorkspaceName;
    for (const auto& name : workspaceOrder_) {
        if (name != workspaceName) {
            nextWorkspaceName = name;
            break;
        }
    }

    if (nextWorkspaceName.empty()) {
        ui_->AddLogOutput("[Workspace Error] Failed to determine replacement workspace.");
        return false;
    }

    if (!workspaceManager_->DeleteWorkspace(workspaceName)) {
        ui_->AddLogOutput("[Workspace Error] Failed to delete workspace '" + workspaceName + "'.");
        return false;
    }

    if (auto compiledIt = compiledPrograms_.find(workspaceName);
        compiledIt != compiledPrograms_.end()) {
        if (compiledIt->second && compiledIt->second != activeProgram_) {
            ShutdownProgramIfInitialized(compiledIt->second);
        }
        compiledPrograms_.erase(compiledIt);
    }

    if (deletingActiveWorkspace) {
        if (activeProgram_) {
            ShutdownProgramIfInitialized(activeProgram_);
        }
        activeProgram_.reset();
    }

    workspaces_.erase(workspaceName);
    workspaceDirty_.erase(workspaceName);
    fileToWorkspace_.erase(workspace.cppPath);
    fileToWorkspace_.erase(workspace.shaderPath);
    latestSources_.erase(workspaceName);
    pendingCompilesAt_.erase(workspaceName);
    compileFailures_.erase(workspaceName);
    compileRetryAfter_.erase(workspaceName);
    recentErrors_.erase(workspaceName);

    ignoreWatcherUntil_.erase(workspace.cppPath);
    ignoreWatcherUntil_.erase(workspace.shaderPath);

    std::erase(workspaceOrder_, workspaceName);

    {
        std::scoped_lock lock(pendingMutex_);
        std::queue<std::string> filteredReloads;
        while (!pendingReloads_.empty()) {
            std::string path = std::move(pendingReloads_.front());
            pendingReloads_.pop();
            if (path == workspace.cppPath || path == workspace.shaderPath) {
                continue;
            }
            filteredReloads.push(std::move(path));
        }
        pendingReloads_.swap(filteredReloads);
    }

    {
        std::scoped_lock lock(compileMutex_);

        std::erase_if(compileJobs_,
                      [&](const CompileJob& job) {
                          return job.workspaceName == workspaceName;
                      });

        std::queue<CompileResult> filteredResults;
        while (!compileResults_.empty()) {
            CompileResult result = std::move(compileResults_.front());
            compileResults_.pop();
            if (result.workspaceName != workspaceName) {
                filteredResults.push(std::move(result));
            }
        }
        compileResults_.swap(filteredResults);
    }

    if (deletingActiveWorkspace) {
        activeWorkspaceName_.clear();
        activeFilePath_.clear();
    }

    SyncWorkspaceUiState();

    if (deletingActiveWorkspace) {
        SwitchToWorkspace(nextWorkspaceName, true);
    }

    ui_->AddLogOutput("[Workspace] Deleted workspace '" + workspaceName + "'.");
    return true;
}

void Engine::SwitchToWorkspace(const std::string& workspaceName, bool focusCppDocument) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    const double nowSeconds = glfwGetTime();
    if (activeWorkspaceName_ != workspaceName) {
        if (auto previousWorkspaceIt = workspaces_.find(activeWorkspaceName_);
            previousWorkspaceIt != workspaces_.end()) {
            auto& previousWorkspace = previousWorkspaceIt->second;
            if (previousWorkspace.activeClockStartSeconds >= 0.0) {
                // Freeze per-workspace time when leaving it; inactive workspaces stay paused.
                const double elapsed = std::max(0.0, nowSeconds - previousWorkspace.activeClockStartSeconds);
                previousWorkspace.accumulatedActiveSeconds += elapsed;
                previousWorkspace.activeClockStartSeconds = -1.0;
            }
        }

        if (activeProgram_) {
            ShutdownProgramIfInitialized(activeProgram_);
        }
        SaveActiveWorkspaceRuntimeState();
        activeProgram_.reset();
        activeWorkspaceName_ = workspaceName;
        LoadWorkspaceRuntimeState(workspaceName);
        workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            return;
        }
        ui_->SetActiveWorkspace(workspaceName);
    }

    if (workspaceIt->second.activeClockStartSeconds < 0.0) {
        workspaceIt->second.activeClockStartSeconds = nowSeconds;
    }

    if (focusCppDocument) {
        activeFilePath_ = workspaceIt->second.cppPath;
        ui_->SetActiveDocument(activeFilePath_);
    }

    if (const bool requiresRecompile = workspaceDirty_[workspaceName]; requiresRecompile) {
        ActivateProgramForWorkspace(workspaceName);
        QueueCompileForWorkspace(workspaceName, glfwGetTime(), true);
        SaveActiveWorkspaceRuntimeState();
        return;
    }

    if (!ActivateProgramForWorkspace(workspaceName)) {
        QueueCompileForWorkspace(workspaceName, glfwGetTime(), true);
    }
    SaveActiveWorkspaceRuntimeState();
}

bool Engine::InitWatcher() {
    workspaceManager_ = std::make_unique<WorkspaceManager>(WORKSPACE_DIR);
    workspaceManager_->Initialize();

    ui_->SetSaveCallback([this](const std::string& path, const std::string& content) {
        const bool ok = workspaceManager_->SaveFile(path, content);
        if (ok) {
            const double now = glfwGetTime();
            ignoreWatcherUntil_[path] = now + WATCHER_IGNORE_SECONDS;
        }
        return ok;
    });

    ui_->SetDocumentChangedCallback([this](const std::string& filepath, const std::string& content) {
        HandleDocumentEdited(filepath, content);
    });

    ui_->SetActiveDocumentChangedCallback([this](const std::string& filepath, const std::string& content) {
        HandleActiveDocumentChanged(filepath, content);
    });

    ui_->SetCreateWorkspaceCallback([this](const std::string& workspaceName) {
        CreateWorkspaceFromUI(workspaceName);
    });

    ui_->SetDeleteWorkspaceCallback([this](const std::string& workspaceName) {
        DeleteWorkspaceFromUI(workspaceName);
    });

    ui_->SetWorkspaceSwitchedCallback([this](const std::string& workspaceName) {
        SwitchToWorkspace(workspaceName, true);
    });

    ui_->SetWorkspaceLineAppendedCallback([this](const std::string& workspaceName,
                                                 const std::string& line,
                                                 bool isConsole) {
        if (!workspaceManager_) {
            return;
        }
        if (isConsole) {
            workspaceManager_->AppendWorkspaceConsoleLog(workspaceName, line);
        } else {
            workspaceManager_->AppendWorkspaceEngineLog(workspaceName, line);
        }
    });

    auto workspaceDescriptors = workspaceManager_->ListWorkspaces();
    for (const auto& descriptor : workspaceDescriptors) {
        RegisterWorkspace(descriptor);
    }

    if (workspaceOrder_.empty()) {
        ui_->AddLogOutput("[Workspace Error] No workspace could be loaded.");
        return false;
    }

    activeWorkspaceName_ = workspaceOrder_.front();
    SyncWorkspaceUiState();
    SwitchToWorkspace(activeWorkspaceName_, true);

    watcher_ = std::make_unique<FileWatcher>(
        WORKSPACE_DIR,
        [this](const std::string& path) { OnFileChanged(path); }
    );
    watcher_->Start();

    return true;
}

void Engine::OnFileChanged(const std::string& filepath) {
    if (const auto extension = std::filesystem::path(filepath).extension().string();
        extension != ".cpp" && extension != ".glsl") {
        return;
    }

    std::scoped_lock lock(pendingMutex_);
    pendingReloads_.push(filepath);
}

void Engine::ProcessPendingReloads(double nowSeconds) {
    std::vector<std::string> filesToProcess;
    {
        std::scoped_lock lock(pendingMutex_);
        while (!pendingReloads_.empty()) {
            filesToProcess.push_back(std::move(pendingReloads_.front()));
            pendingReloads_.pop();
        }
    }

    for (const auto& filepath : filesToProcess) {
        auto ignoredIt = ignoreWatcherUntil_.find(filepath);
        if (ignoredIt != ignoreWatcherUntil_.end() && nowSeconds < ignoredIt->second) {
            continue;
        }

        const auto content = workspaceManager_->ReadFile(filepath);
        if (!content.has_value()) {
            ui_->AddLogOutput("[Watcher] Failed to read changed file: " + filepath);
            continue;
        }

        const std::string workspaceName = WorkspaceForPath(filepath);
        if (workspaceName.empty()) {
            continue;
        }

        auto workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            continue;
        }

        const bool isCpp = (filepath == workspaceIt->second.cppPath);
        if (const bool unchanged = isCpp ? (workspaceIt->second.cppSource == *content)
                                         : (workspaceIt->second.shaderSource == *content);
            unchanged) {
            continue;
        }

        UpdateWorkspaceSourceFromDocument(workspaceName, filepath, *content);
        ui_->UpdateDocumentContent(filepath, *content);
        if (workspaceName == activeWorkspaceName_) {
            QueueCompileForWorkspace(workspaceName, nowSeconds, true);
        }
        ui_->AddLogOutput("[Watcher] External change detected: " + filepath);
    }
}

void Engine::HandleDocumentEdited(const std::string& filepath, const std::string& content) {
    const std::string workspaceName = WorkspaceForPath(filepath);
    if (workspaceName.empty()) {
        return;
    }

    UpdateWorkspaceSourceFromDocument(workspaceName, filepath, content);
    if (workspaceName == activeWorkspaceName_) {
        QueueCompileForWorkspace(workspaceName, glfwGetTime(), false);
    }
}

void Engine::HandleActiveDocumentChanged(const std::string& filepath, const std::string& content) {
    activeFilePath_ = filepath;
    const std::string workspaceName = WorkspaceForPath(filepath);
    if (workspaceName.empty()) {
        return;
    }

    UpdateWorkspaceSourceFromDocument(workspaceName, filepath, content);
    if (workspaceName != activeWorkspaceName_) {
        SwitchToWorkspace(workspaceName, false);
        return;
    }

    if (!ActivateProgramForWorkspace(workspaceName)) {
        QueueCompileForWorkspace(workspaceName, glfwGetTime(), true);
    }
}

void Engine::QueueCompile(const std::string& workspaceName, const std::string& source, double nowSeconds, bool immediate) {
    const std::size_t sourceHash = std::hash<std::string>{}(source);
    auto failureIt = compileFailures_.find(workspaceName);
    if (failureIt != compileFailures_.end() && failureIt->second.sourceHash == sourceHash) {
        if (!failureIt->second.suppressionLogged) {
            ui_->AddLogOutput("[JIT] Skipping unchanged invalid source for workspace '" + workspaceName +
                              "'. Waiting for edits.");
            failureIt->second.suppressionLogged = true;
        }
        pendingCompilesAt_.erase(workspaceName);
        return;
    }
    if (failureIt != compileFailures_.end() && failureIt->second.sourceHash != sourceHash) {
        compileFailures_.erase(failureIt);
    }

    latestSources_[workspaceName] = source;
    // Error-heavy workspaces get a longer debounce to avoid compile spam while user is fixing errors.
    double debounce = MIN_DEBOUNCE_SECONDS;
    auto errorIt = recentErrors_.find(workspaceName);
    if (errorIt != recentErrors_.end()) {
        auto& history = errorIt->second;
        while (!history.empty() && (nowSeconds - history.front() > ERROR_HISTORY_SECONDS)) {
            history.pop_front();
        }
        debounce += static_cast<double>(history.size()) * DEBOUNCE_STEP_SECONDS;
        if (debounce > MAX_DEBOUNCE_SECONDS) debounce = MAX_DEBOUNCE_SECONDS;
    }

    double requestedAt = immediate ? nowSeconds : (nowSeconds + debounce);
    auto retryIt = compileRetryAfter_.find(workspaceName);
    if (retryIt != compileRetryAfter_.end() && requestedAt < retryIt->second) {
        requestedAt = retryIt->second;
    }
    pendingCompilesAt_[workspaceName] = requestedAt;
}

std::vector<std::string> Engine::CollectDueCompiles(double nowSeconds) const {
    std::vector<std::string> duePaths;
    for (const auto& pending : pendingCompilesAt_) {
        if (pending.second <= nowSeconds) {
            duePaths.emplace_back(pending.first);
        }
    }
    return duePaths;
}

Engine::InFlightStatus Engine::EvaluateInFlightStatus(double nowSeconds, bool includeQueuedJobs) {
    InFlightStatus status{};
    {
        std::scoped_lock lock(compileMutex_);
        status.inFlight = !inFlightSourceHashes_.empty();
        if (includeQueuedJobs) {
            status.inFlight = status.inFlight || !compileJobs_.empty();
        }
        if (status.inFlight && inFlightStartTime_ > 0.0) {
            const double inFlightSeconds = nowSeconds - inFlightStartTime_;
            status.stalled = (inFlightSeconds > 5.0);
            status.shouldReset = (inFlightSeconds > 10.0);
        }
    }
    return status;
}

void Engine::EnqueueDueCompiles(const std::vector<std::string>& duePaths) {
    std::scoped_lock lock(compileMutex_);
    for (const auto& workspaceName : duePaths) {
        auto sourceIt = latestSources_.find(workspaceName);
        if (sourceIt == latestSources_.end()) {
            continue;
        }

        const std::size_t sourceHash = std::hash<std::string>{}(sourceIt->second);
        if (auto inFlightIt = inFlightSourceHashes_.find(workspaceName);
            inFlightIt != inFlightSourceHashes_.end() && inFlightIt->second == sourceHash) {
            pendingCompilesAt_.erase(workspaceName);
            continue;
        }

        // Avoid duplicate work when the same source hash is already queued/in-flight.
        const bool alreadyQueued = std::any_of(compileJobs_.begin(),
                                               compileJobs_.end(),
                                               [&](const CompileJob& job) {
                                                   return job.workspaceName == workspaceName &&
                                                          job.sourceHash == sourceHash;
                                               });
        if (alreadyQueued) {
            pendingCompilesAt_.erase(workspaceName);
            continue;
        }

        auto workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            pendingCompilesAt_.erase(workspaceName);
            continue;
        }

        std::erase_if(compileJobs_,
                      [&](const CompileJob& job) {
                          return job.workspaceName == workspaceName;
                      });
        compileJobs_.emplace_back(CompileJob{
            workspaceName,
            workspaceIt->second.cppPath,
            sourceIt->second,
            sourceHash
        });
        pendingCompilesAt_.erase(workspaceName);
    }
}

void Engine::SubmitDueCompiles(double nowSeconds) {
    if (resetRequested_) {
        ui_->SetCompilationStatus(true, ActiveWorkspaceHasCompileError(), true);
        return;
    }

    const std::vector<std::string> duePaths = CollectDueCompiles(nowSeconds);

    if (duePaths.empty()) {
        const InFlightStatus status = EvaluateInFlightStatus(nowSeconds, true);
        if (status.shouldReset) {
            ResetJIT();
            ui_->SetCompilationStatus(true, ActiveWorkspaceHasCompileError(), true);
            return;
        }

        ui_->SetCompilationStatus(status.inFlight, ActiveWorkspaceHasCompileError(), status.stalled);
        return;
    }

    const InFlightStatus status = EvaluateInFlightStatus(nowSeconds, false);
    if (status.shouldReset) {
        ResetJIT();
        ui_->SetCompilationStatus(true, ActiveWorkspaceHasCompileError(), true);
        return;
    }

    EnqueueDueCompiles(duePaths);
    compileCv_.notify_one();
    ui_->SetCompilationStatus(true, ActiveWorkspaceHasCompileError(), status.stalled);
}

void Engine::CompileThreadMain(std::shared_ptr<std::atomic<bool>> running) {
    auto currentJit = jit_;
    while (running->load()) {
        CompileJob job;
        {
            std::unique_lock<std::mutex> lock(compileMutex_);
            compileCv_.wait(lock, [this, running]() {
                return !running->load() || !compileJobs_.empty();
            });

            if (!running->load()) {
                break;
            }

            job = std::move(compileJobs_.front());
            compileJobs_.pop_front();
            inFlightSourceHashes_[job.workspaceName] = job.sourceHash;
            inFlightStartTime_ = glfwGetTime();
        }

        // Heavy JIT/Clang work happens off the UI thread.
        auto program = currentJit->CompileSource(job.sourceName, job.source);
        {
            std::scoped_lock lock(compileMutex_);
            inFlightSourceHashes_.erase(job.workspaceName);
            inFlightStartTime_ = 0.0;
            compileResults_.push(CompileResult{ job.workspaceName, std::move(program), job.sourceHash });
        }
    }
}

void Engine::InitializeProgramIfNeeded(const std::shared_ptr<JitProgram>& program) {
    if (!program || program->initialized) {
        return;
    }

    if (program->functions.init) {
        program->functions.init(&ctx_);
    }
    program->initialized = true;
}

void Engine::ShutdownProgramIfInitialized(const std::shared_ptr<JitProgram>& program) {
    if (!program || !program->initialized) {
        return;
    }

    if (program->functions.shutdown) {
        program->functions.shutdown(&ctx_);
    }
    program->initialized = false;
}

bool Engine::ActivateProgramForWorkspace(const std::string& workspaceName) {
    auto it = compiledPrograms_.find(workspaceName);
    if (it == compiledPrograms_.end()) {
        return false;
    }

    const auto& nextProgram = it->second;
    if (activeProgram_ && activeProgram_ != nextProgram) {
        ShutdownProgramIfInitialized(activeProgram_);
    }

    activeProgram_ = nextProgram;
    InitializeProgramIfNeeded(activeProgram_);

    ui_->AddLogOutput("[Runtime] Active workspace switched to " + workspaceName);
    return true;
}

bool Engine::ExportActiveWorkspace(const std::string& targetPath) const {
    if (activeWorkspaceName_.empty()) return false;
    return workspaceManager_->ExportWorkspace(activeWorkspaceName_, targetPath);
}

bool Engine::ImportWorkspace(const std::string& sourcePath) {
    auto importedName = workspaceManager_->ImportWorkspace(sourcePath);
    if (!importedName) return false;

    auto descriptor = workspaceManager_->GetWorkspace(*importedName);
    if (!descriptor) return false;

    if (RegisterWorkspace(*descriptor)) {
        SyncWorkspaceUiState();
        SwitchToWorkspace(*importedName, true);
        return true;
    }
    return false;
}

bool Engine::ImportWorkspacePackage(const std::string& packageData, const std::string& sourceHint) {
    if (!workspaceManager_) {
        return false;
    }

    auto importedName = workspaceManager_->ImportWorkspacePackage(packageData, sourceHint);
    if (!importedName) {
        return false;
    }

    auto descriptor = workspaceManager_->GetWorkspace(*importedName);
    if (!descriptor) {
        return false;
    }

    if (!RegisterWorkspace(*descriptor)) {
        return false;
    }

    SyncWorkspaceUiState();
    SwitchToWorkspace(*importedName, true);
    return true;
}

bool Engine::InitLanShare() {
    if (!ui_ || !workspaceManager_) {
        return false;
    }

    lanShare_ = std::make_unique<LanWorkspaceShareService>();
    if (!lanShare_->Start()) {
        const auto diagnostics = lanShare_->SnapshotDiagnostics();
        if (!diagnostics.lastError.empty()) {
            ui_->AddLogOutput("[Network Error] " + diagnostics.lastError);
        }
        return false;
    }

    ui_->AddLogOutput("[Network] LAN sharing active as '" + lanShare_->LocalDisplayName() + "'.");
    return true;
}

void Engine::UpdateLanShareUiState() {
    if (!ui_) {
        return;
    }

    if (!lanShare_) {
        EditorUI::NetworkDiagnostics diagnostics;
        diagnostics.lastError = "LAN sharing service was not initialized.";
        ui_->SetNetworkDiagnostics(std::move(diagnostics));
        return;
    }

    const auto snapshot = lanShare_->SnapshotDiagnostics();
    EditorUI::NetworkDiagnostics diagnostics;
    diagnostics.serviceRunning = snapshot.serviceRunning;
    diagnostics.udpSocketBound = snapshot.udpSocketBound;
    diagnostics.tcpSocketBound = snapshot.tcpSocketBound;
    diagnostics.multicastJoinAttempted = snapshot.multicastJoinAttempted;
    diagnostics.multicastJoinSucceeded = snapshot.multicastJoinSucceeded;
    diagnostics.winsockInitialized = snapshot.winsockInitialized;
    diagnostics.discoveryPort = snapshot.discoveryPort;
    diagnostics.transferPort = snapshot.transferPort;
    diagnostics.localPeerId = snapshot.localPeerId;
    diagnostics.localDisplayName = snapshot.localDisplayName;
    diagnostics.discoveryMulticastAddress = snapshot.discoveryMulticastAddress;
    diagnostics.lastError = snapshot.lastError;
    diagnostics.lastUdpSenderIp = snapshot.lastUdpSenderIp;
    diagnostics.localIpv4Addresses = snapshot.localIpv4Addresses;
    diagnostics.directedBroadcastAddresses = snapshot.directedBroadcastAddresses;
    diagnostics.unicastProbeTargetCount = snapshot.unicastProbeTargetCount;
    diagnostics.nowSeconds = snapshot.nowSeconds;
    diagnostics.lastUdpSentSeconds = snapshot.lastUdpSentSeconds;
    diagnostics.lastUdpReceivedSeconds = snapshot.lastUdpReceivedSeconds;
    diagnostics.lastHelloSentSeconds = snapshot.lastHelloSentSeconds;
    diagnostics.lastHelloReceivedSeconds = snapshot.lastHelloReceivedSeconds;
    diagnostics.udpPacketsSent = snapshot.udpPacketsSent;
    diagnostics.udpPacketsSendFailed = snapshot.udpPacketsSendFailed;
    diagnostics.udpPacketsReceived = snapshot.udpPacketsReceived;
    diagnostics.helloSentCount = snapshot.helloSentCount;
    diagnostics.helloReceivedCount = snapshot.helloReceivedCount;
    diagnostics.offersSentCount = snapshot.offersSentCount;
    diagnostics.offersReceivedCount = snapshot.offersReceivedCount;
    diagnostics.outgoingFetchAttempts = snapshot.outgoingFetchAttempts;
    diagnostics.outgoingFetchSuccesses = snapshot.outgoingFetchSuccesses;
    diagnostics.outgoingFetchFailures = snapshot.outgoingFetchFailures;
    diagnostics.incomingTransferRequests = snapshot.incomingTransferRequests;
    diagnostics.incomingTransferSuccesses = snapshot.incomingTransferSuccesses;
    diagnostics.incomingTransferFailures = snapshot.incomingTransferFailures;
    diagnostics.peersKnown = snapshot.peersKnown;
    diagnostics.pendingIncomingOffers = snapshot.pendingIncomingOffers;
    diagnostics.pendingOutgoingPackets = snapshot.pendingOutgoingPackets;
    diagnostics.cachedSharedPayloads = snapshot.cachedSharedPayloads;
    ui_->SetNetworkDiagnostics(std::move(diagnostics));

    std::vector<EditorUI::NetworkPeer> peerViews;
    const auto peers = lanShare_->SnapshotPeers();
    peerViews.reserve(peers.size());
    for (const auto& peer : peers) {
        peerViews.push_back(EditorUI::NetworkPeer{
            peer.id,
            peer.displayName,
            peer.ipAddress
        });
    }
    ui_->SetNetworkPeers(std::move(peerViews));

    const auto incomingOffers = lanShare_->DrainIncomingOffers();
    for (const auto& offer : incomingOffers) {
        if (pendingLanOffersById_.contains(offer.offerId)) {
            continue;
        }
        pendingLanOffersById_[offer.offerId] = offer;
        ui_->QueueIncomingWorkspaceShareOffer(EditorUI::IncomingWorkspaceShareOffer{
            offer.offerId,
            offer.workspaceName,
            offer.senderName
        });
    }
}

void Engine::HandleShareWorkspaceRequest(const std::vector<std::string>& targetPeerIds, bool shareToAll) {
    if (!lanShare_ || !workspaceManager_) {
        ui_->AddLogOutput("[Network Error] LAN sharing service is not running.");
        return;
    }
    if (activeWorkspaceName_.empty()) {
        ui_->AddLogOutput("[Network Error] No active workspace selected.");
        return;
    }
    if (!shareToAll && targetPeerIds.empty()) {
        ui_->AddLogOutput("[Network Error] No target computers selected.");
        return;
    }

    auto packageData = workspaceManager_->ExportWorkspacePackage(activeWorkspaceName_);
    if (!packageData.has_value()) {
        ui_->AddLogOutput("[Network Error] Failed to package workspace '" + activeWorkspaceName_ + "' for sharing.");
        return;
    }

    const bool shared = lanShare_->ShareWorkspacePackage(activeWorkspaceName_, *packageData, targetPeerIds, shareToAll);
    if (!shared) {
        ui_->AddLogOutput("[Network Error] Failed to send workspace share offer.");
        return;
    }

    if (shareToAll) {
        ui_->AddLogOutput("[Network] Shared workspace '" + activeWorkspaceName_ + "' to all discovered computers.");
    } else {
        ui_->AddLogOutput("[Network] Shared workspace '" + activeWorkspaceName_ +
                          "' to " + std::to_string(targetPeerIds.size()) + " selected computer(s).");
    }
}

void Engine::HandleWorkspaceShareDecision(const std::string& offerId, bool accepted) {
    auto offerIt = pendingLanOffersById_.find(offerId);
    if (offerIt == pendingLanOffersById_.end()) {
        return;
    }

    const LanWorkspaceOffer offer = offerIt->second;
    pendingLanOffersById_.erase(offerIt);

    if (!accepted) {
        ui_->AddLogOutput("[Network] Declined workspace '" + offer.workspaceName + "' from '" + offer.senderName + "'.");
        return;
    }

    if (!lanShare_) {
        ui_->AddLogOutput("[Network Error] LAN sharing service is not running.");
        return;
    }

    auto packageData = lanShare_->FetchWorkspacePackage(offer);
    if (!packageData.has_value()) {
        const auto diagnostics = lanShare_->SnapshotDiagnostics();
        std::string errorSuffix;
        if (!diagnostics.lastError.empty()) {
            errorSuffix = " Reason: " + diagnostics.lastError;
        }
        ui_->AddLogOutput("[Network Error] Failed to receive workspace '" + offer.workspaceName +
                          "' from '" + offer.senderName + "'." + errorSuffix);
        return;
    }

    if (!ImportWorkspacePackage(*packageData, offer.workspaceName)) {
        ui_->AddLogOutput("[Network Error] Failed to import received workspace '" + offer.workspaceName + "'.");
        return;
    }

    ui_->AddLogOutput("[Network] Loaded workspace '" + offer.workspaceName + "' from '" + offer.senderName + "'.");
}

void Engine::HandleRequestFirewallAccess() {
    if (!ui_) {
        return;
    }

#if defined(__linux__)
    ui_->AddLogOutput("[Network] Requesting administrator permission to open UDP 39541 and TCP 39542...");

    if (RunShellCommand("command -v pkexec >/dev/null 2>&1") != 0) {
        ui_->AddLogOutput("[Network Error] 'pkexec' is not available. Install polkit to enable automatic firewall setup.");
        return;
    }

    const std::string command =
        "pkexec sh -lc '"
        "if command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld; then "
        "firewall-cmd --add-port=39541/udp >/dev/null && "
        "firewall-cmd --add-port=39542/tcp >/dev/null && "
        "firewall-cmd --permanent --add-port=39541/udp >/dev/null && "
        "firewall-cmd --permanent --add-port=39542/tcp >/dev/null && "
        "firewall-cmd --reload >/dev/null; "
        "exit $?; "
        "fi; "
        "if command -v ufw >/dev/null 2>&1; then "
        "ufw allow 39541/udp >/dev/null && "
        "ufw allow 39542/tcp >/dev/null; "
        "exit $?; "
        "fi; "
        "if command -v nft >/dev/null 2>&1 && nft list table inet filter >/dev/null 2>&1; then "
        "nft add rule inet filter input udp dport 39541 accept >/dev/null 2>&1 || true; "
        "nft add rule inet filter input tcp dport 39542 accept >/dev/null 2>&1 || true; "
        "exit 0; "
        "fi; "
        "exit 125'";

    const int exitCode = RunShellCommand(command);
    if (exitCode == 0) {
        ui_->AddLogOutput("[Network] Firewall ports opened: UDP 39541, TCP 39542.");
        return;
    }
    if (exitCode == 125) {
        ui_->AddLogOutput("[Network Error] No supported firewall manager detected (firewalld/ufw/nftables).");
        return;
    }
    if (exitCode == 126 || exitCode == 127) {
        ui_->AddLogOutput("[Network Error] Firewall setup was cancelled or blocked by policy.");
        return;
    }
    ui_->AddLogOutput("[Network Error] Automatic firewall setup failed (exit code " + std::to_string(exitCode) + ").");
#elif defined(_WIN32)
    ui_->AddLogOutput("[Network] Requesting administrator permission to open UDP 39541 and TCP 39542...");

    const std::string elevatedScript =
        "netsh advfirewall firewall add rule name='JITGL LAN UDP 39541' dir=in action=allow protocol=UDP localport=39541 profile=private | Out-Null; "
        "netsh advfirewall firewall add rule name='JITGL LAN TCP 39542' dir=in action=allow protocol=TCP localport=39542 profile=private | Out-Null; "
        "exit $LASTEXITCODE";
    const std::string escapedElevatedScript = EscapeForPowerShellSingleQuoted(elevatedScript);
    const std::string command =
        "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
        "try { "
        "$p = Start-Process -FilePath 'powershell' -Verb RunAs -Wait -PassThru "
        "-ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-Command','" + escapedElevatedScript + "'); "
        "if ($null -eq $p) { exit 1 } else { exit $p.ExitCode } "
        "} catch { exit 126 }\"";

    const int exitCode = RunShellCommand(command);
    if (exitCode == 0) {
        ui_->AddLogOutput("[Network] Windows Firewall rules added for UDP 39541 and TCP 39542.");
        return;
    }
    if (exitCode == 126) {
        ui_->AddLogOutput("[Network Error] Firewall setup was cancelled or blocked by policy.");
        return;
    }
    ui_->AddLogOutput("[Network Error] Automatic firewall setup failed (exit code " + std::to_string(exitCode) + ").");
#elif defined(__APPLE__)
    ui_->AddLogOutput("[Network] Requesting administrator permission to allow this app through macOS firewall...");

    if (RunShellCommand("test -x /usr/libexec/ApplicationFirewall/socketfilterfw") != 0) {
        ui_->AddLogOutput("[Network Error] macOS firewall helper not found: /usr/libexec/ApplicationFirewall/socketfilterfw");
        return;
    }

    const std::string executablePath = CurrentExecutablePath();
    if (executablePath.empty()) {
        ui_->AddLogOutput("[Network Error] Failed to resolve application executable path.");
        return;
    }

    const std::string quotedExecutable = QuoteForPosixShell(executablePath);
    const std::string shellScript =
        "/usr/libexec/ApplicationFirewall/socketfilterfw --add " + quotedExecutable + " >/dev/null 2>&1; "
        "/usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp " + quotedExecutable + " >/dev/null 2>&1";
    const std::string appleScriptCommand =
        "osascript -e \"do shell script \\\"" + EscapeForAppleScriptDoubleQuoted(shellScript) +
        "\\\" with administrator privileges\"";

    const int exitCode = RunShellCommand(appleScriptCommand);
    if (exitCode == 0) {
        ui_->AddLogOutput("[Network] macOS firewall updated to allow this app (UDP/TCP LAN sharing).");
        return;
    }
    if (exitCode == 1 || exitCode == 126 || exitCode == 127) {
        ui_->AddLogOutput("[Network Error] Firewall setup was cancelled or blocked by policy.");
        return;
    }
    ui_->AddLogOutput("[Network Error] Automatic firewall setup failed (exit code " + std::to_string(exitCode) + ").");
#else
    ui_->AddLogOutput("[Network] Automatic firewall configuration is not implemented for this operating system.");
#endif
}

void Engine::HandleCompileFailure(const CompileResult& result) {
    ui_->AddLogOutput("[JIT] Compile failed for workspace '" + result.workspaceName +
                      "'. Keeping previous program.");
    if (auto sourceIt = latestSources_.find(result.workspaceName);
        sourceIt != latestSources_.end()) {
        const std::size_t currentHash = std::hash<std::string>{}(sourceIt->second);
        if (currentHash == result.sourceHash) {
            compileFailures_[result.workspaceName] = CompileFailureState{ result.sourceHash, false };
        }
    }

    // Exponential-ish retry delay dampens repeated failing recompiles.
    const double now = glfwGetTime();
    auto& history = recentErrors_[result.workspaceName];
    history.push_back(now);
    while (!history.empty() && (now - history.front() > ERROR_HISTORY_SECONDS)) {
        history.pop_front();
    }

    double debounce = MIN_DEBOUNCE_SECONDS + static_cast<double>(history.size()) * DEBOUNCE_STEP_SECONDS;
    if (debounce > MAX_DEBOUNCE_SECONDS) {
        debounce = MAX_DEBOUNCE_SECONDS;
    }

    compileRetryAfter_[result.workspaceName] = now + debounce;
    ui_->SetCompilationStatus(false, true, false);
}

void Engine::HandleCompileSuccess(const CompileResult& result) {
    ui_->SetCompilationStatus(false, ActiveWorkspaceHasCompileError(), false);
    recentErrors_.erase(result.workspaceName);

    if (auto latestIt = latestSources_.find(result.workspaceName);
        latestIt != latestSources_.end()) {
        const std::size_t latestHash = std::hash<std::string>{}(latestIt->second);
        if (latestHash == result.sourceHash) {
            workspaceDirty_[result.workspaceName] = false;
        }
    }

    if (auto failureIt = compileFailures_.find(result.workspaceName);
        failureIt != compileFailures_.end() && failureIt->second.sourceHash == result.sourceHash) {
        compileFailures_.erase(failureIt);
    }
    compileRetryAfter_.erase(result.workspaceName);

    if (auto existingIt = compiledPrograms_.find(result.workspaceName);
        existingIt != compiledPrograms_.end() && existingIt->second != result.program) {
        ShutdownProgramIfInitialized(existingIt->second);
    }

    compiledPrograms_[result.workspaceName] = result.program;
    if (result.workspaceName != activeWorkspaceName_) {
        return;
    }

    if (activeProgram_ && activeProgram_ != result.program) {
        ShutdownProgramIfInitialized(activeProgram_);
    }
    activeProgram_ = result.program;
    ctx_.reloadCount++;
    InitializeProgramIfNeeded(activeProgram_);
    SaveActiveWorkspaceRuntimeState();
}

void Engine::ProcessCompileResults() {
    std::queue<CompileResult> localResults;
    {
        std::scoped_lock lock(compileMutex_);
        std::swap(localResults, compileResults_);
    }

    // Drain queue on main thread so GL/JIT state transitions stay single-threaded.
    while (!localResults.empty()) {
        CompileResult result = std::move(localResults.front());
        localResults.pop();

        if (!workspaces_.contains(result.workspaceName)) {
            continue;
        }
        if (!result.program) {
            HandleCompileFailure(result);
            continue;
        }

        HandleCompileSuccess(result);
    }
}

void Engine::RenderSceneToTexture() {
    if (sceneFbo_ == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, sceneWidth_, sceneHeight_);

    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (activeProgram_) {
        const auto& fn = activeProgram_->functions;
        if (fn.update) {
            fn.update(&ctx_);
        }
        if (fn.render) {
            fn.render(&ctx_);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Engine::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        ProcessPendingReloads(now);
        CompletePendingJITReset();
        SubmitDueCompiles(now);
        ProcessCompileResults();

        ctx_.deltaTime = static_cast<float>(now - lastTime_);
        ctx_.time = 0.0f;
        if (auto activeWorkspaceIt = workspaces_.find(activeWorkspaceName_);
            activeWorkspaceIt != workspaces_.end()) {
            auto& activeWorkspace = activeWorkspaceIt->second;
            if (activeWorkspace.activeClockStartSeconds < 0.0) {
                activeWorkspace.activeClockStartSeconds = now;
            }
            const double elapsed = std::max(0.0, now - activeWorkspace.activeClockStartSeconds);
            ctx_.time = static_cast<float>(activeWorkspace.accumulatedActiveSeconds + elapsed);
        }
        ctx_.frameCount++;
        lastTime_ = now;

        int windowW = 0;
        int windowH = 0;
        glfwGetFramebufferSize(window_, &windowW, &windowH);

        if (EnsureSceneRenderTarget(windowW, windowH)) {
            ctx_.width = sceneWidth_;
            ctx_.height = sceneHeight_;
        } else {
            ctx_.width = windowW;
            ctx_.height = windowH;
        }

        RenderSceneToTexture();
        UpdateLanShareUiState();

        ui_->NewFrame();
        ui_->Draw();

        glViewport(0, 0, windowW, windowH);
        glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ui_->Render();
        glfwSwapBuffers(window_);
    }
}

void Engine::Shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;

    if (ui_) {
        ui_->AddLogOutput("[Engine] Shutting down...");
    }

    if (watcher_) {
        watcher_->Stop();
        watcher_.reset();
    }

    if (lanShare_) {
        lanShare_->Stop();
        lanShare_.reset();
    }
    pendingLanOffersById_.clear();

    bool detachedCompileThread = false;
    if (compileThreadRunning_) {
        compileThreadRunning_->store(false);
    }
    compileCv_.notify_all();
    if (compileThread_.joinable()) {
        // If we are stalling, don't wait forever
        if (inFlightStartTime_ > 0.0 && (glfwGetTime() - inFlightStartTime_ > 5.0)) {
            if (ui_) ui_->AddLogOutput("[Engine] Compile thread stalled during shutdown. Detaching.");
            compileThread_.detach();
            detachedCompileThread = true;
        } else {
            compileThread_.join();
        }
    }

    {
        std::scoped_lock lock(compileMutex_);
        compileJobs_.clear();
        compileResults_ = std::queue<CompileResult>{};
        inFlightSourceHashes_.clear();
    }

    for (const auto& [workspaceName, program] : compiledPrograms_) {
        (void)workspaceName;
        ShutdownProgramIfInitialized(program);
    }
    ShutdownProgramIfInitialized(activeProgram_);

    // IMPORTANT: Clear all JIT programs (and their interpreters)
    // BEFORE terminating the JIT engine (which shuts down LLVM).
    activeProgram_.reset();
    compiledPrograms_.clear();

    compileFailures_.clear();
    compileRetryAfter_.clear();
    recentErrors_.clear();
    ignoreWatcherUntil_.clear();
    latestSources_.clear();
    pendingCompilesAt_.clear();
    inFlightSourceHashes_.clear();
    workspaces_.clear();
    workspaceOrder_.clear();
    fileToWorkspace_.clear();
    workspaceDirty_.clear();
    activeWorkspaceName_.clear();
    activeFilePath_.clear();

    if (jit_) {
        if (!detachedCompileThread) {
            jit_->Terminate();
        } else {
            jit_->SetOutputCallback(nullptr);
        }
        jit_.reset();
    }

    if (consoleRedirect_) {
        consoleRedirect_->Stop();
        consoleRedirect_.reset();
    }

    if (workspaceManager_) {
        workspaceManager_.reset();
    }

    DestroySceneRenderTarget();

    if (ctx_.vao) {
        glDeleteVertexArrays(1, &ctx_.vao);
        ctx_.vao = 0;
    }

    if (ctx_.vbo) {
        glDeleteBuffers(1, &ctx_.vbo);
        ctx_.vbo = 0;
    }

    if (ctx_.defaultShader) {
        glDeleteProgram(ctx_.defaultShader);
        ctx_.defaultShader = 0;
    }

    // Note: userData is left to the user to manage via shutdown() if they allocated it,
    // but we clear the pointer here for safety.
    ctx_.userData = nullptr;

    if (ui_) {
        ui_->Shutdown();
        ui_.reset();
    }

    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    glfwTerminate();
}
