#include "core/Engine.h"
#include "ui/EditorUI.h"
#include "watcher/FileWatcher.h"
#include "system/ConsoleRedirectSession.h"
#include "core/WorkspaceManager.h"
#include "runtime/EngineContext.h"
#include "jit/JitEngine.h"
#include "uniform/UniformParser.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif
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
#ifndef JIT_PROJECT_SOURCE_DIR
#define JIT_PROJECT_SOURCE_DIR "."
#endif

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr const char* WINDOW_TITLE = "JITGL";

    constexpr int GL_VERSION_MAJOR = 3;
    constexpr int GL_VERSION_MINOR = 3;

    constexpr const char* WORKSPACE_DIR = JIT_PROJECT_BINARY_DIR "/workspace";
    constexpr const char* PREAMBLE_PATH = JIT_PROJECT_BINARY_DIR "/runtime/engine.hpp";
    constexpr const char* SHOWCASE_WORKSPACE_NAME = "showcase";
    constexpr const char* SHOWCASE_SCENE_ASSET_PATH = JIT_PROJECT_BINARY_DIR "/assets/showcase_scene.cpp";
    constexpr const char* SHOWCASE_SHADER_ASSET_PATH = JIT_PROJECT_BINARY_DIR "/assets/showcase_shader.glsl";
    constexpr const char* SHOWCASE_UNIFORMS_ASSET_PATH = JIT_PROJECT_BINARY_DIR "/assets/showcase_uniforms.json";
    constexpr const char* SHOWCASE_SCENE_ASSET_FALLBACK_PATH = JIT_PROJECT_SOURCE_DIR "/assets/showcase_scene.cpp";
    constexpr const char* SHOWCASE_SHADER_ASSET_FALLBACK_PATH = JIT_PROJECT_SOURCE_DIR "/assets/showcase_shader.glsl";
    constexpr const char* SHOWCASE_UNIFORMS_ASSET_FALLBACK_PATH = JIT_PROJECT_SOURCE_DIR "/assets/showcase_uniforms.json";

    constexpr double MIN_DEBOUNCE_SECONDS = 0.15;
    constexpr double MAX_DEBOUNCE_SECONDS = 1.95;
    constexpr double DEBOUNCE_STEP_SECONDS = 0.2;
    constexpr double WATCHER_IGNORE_SECONDS = 1.0;
    constexpr double ERROR_HISTORY_SECONDS = 5.0;
    constexpr std::array<float, 12> FULLSCREEN_QUAD_VERTICES = {
        -1.f, -1.f, 1.f, -1.f, 1.f, 1.f,
        -1.f, -1.f, 1.f, 1.f, -1.f, 1.f,
    };
    constexpr bool VERBOSE_WORKSPACE_DEBUG = true;

    void DebugLog(const std::string& text) {
        if (!VERBOSE_WORKSPACE_DEBUG) {
            return;
        }
#if defined(__linux__) || defined(__APPLE__)
        static int ttyFd = -2;
        if (ttyFd == -2) {
            ttyFd = open("/dev/tty", O_WRONLY | O_CLOEXEC);
        }
        if (ttyFd < 0) {
            return;
        }

        const std::string line = "[JITGL DBG] " + text + '\n';
        (void)write(ttyFd, line.data(), line.size());
#elif defined(_WIN32)
        const std::string line = "[JITGL DBG] " + text + '\n';
        OutputDebugStringA(line.c_str());
#endif
    }

    std::string FormatStateSlice(const std::array<uint32_t, 64>& stateI,
                                 const std::array<float, 64>& stateF) {
        std::ostringstream out;
        out << "stateI={";
        for (int i = 0; i < 7; ++i) {
            if (i > 0) {
                out << ",";
            }
            out << stateI[static_cast<std::size_t>(i)];
        }
        out << "} stateF={";
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                out << ",";
            }
            out << std::fixed << std::setprecision(3) << stateF[static_cast<std::size_t>(i)];
        }
        out << "}";
        return out.str();
    }

    std::optional<std::string> LoadTextFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::nullopt;
        }
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::optional<std::string> LoadTextFileWithFallback(const char* primaryPath, const char* fallbackPath) {
        if (auto content = LoadTextFile(primaryPath); content.has_value()) {
            return content;
        }
        return LoadTextFile(fallbackPath);
    }

    std::string trim(std::string value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    std::string EscapeJsonString(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (const char c : value) {
            switch (c) {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped.push_back(c); break;
            }
        }
        return escaped;
    }

    struct ShaderSections {
        std::string vertex;
        std::string fragment;
        std::string compute;
    };

    bool parseShaderSections(const std::string& shaderSource, ShaderSections* outSections) {
        if (outSections == nullptr) {
            return false;
        }
        outSections->vertex.clear();
        outSections->fragment.clear();
        outSections->compute.clear();

        enum class Section {
            None,
            Vertex,
            Fragment,
            Compute,
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
                } else if (trimmedLine.find("compute") != std::string::npos) {
                    currentSection = Section::Compute;
                } else {
                    currentSection = Section::None;
                }
            } else {
                if (currentSection == Section::Vertex) {
                    outSections->vertex += line;
                    outSections->vertex += '\n';
                } else if (currentSection == Section::Fragment) {
                    outSections->fragment += line;
                    outSections->fragment += '\n';
                } else if (currentSection == Section::Compute) {
                    outSections->compute += line;
                    outSections->compute += '\n';
                }
            }

            if (lineEnd == std::string::npos) {
                break;
            }
            cursor = lineEnd + 1;
        }

        const bool hasGraphics = !outSections->vertex.empty() && !outSections->fragment.empty();
        const bool hasCompute = !outSections->compute.empty();
        return hasGraphics || hasCompute;
    }

    std::string NormalizePathString(const std::filesystem::path& path) {
        return path.lexically_normal().string();
    }

    std::string PipelineSlotKey(const std::string& workspaceName, const std::string& slotName) {
        return workspaceName + "::" + slotName;
    }

    bool ParseIncludeDirective(const std::string& line, std::string* outIncludeName) {
        if (outIncludeName == nullptr) {
            return false;
        }
        const std::string trimmed = trim(line);
        if (trimmed.rfind("#include", 0) != 0) {
            return false;
        }

        const std::size_t firstQuote = trimmed.find('"');
        if (firstQuote == std::string::npos) {
            return false;
        }
        const std::size_t secondQuote = trimmed.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos || secondQuote <= firstQuote + 1) {
            return false;
        }

        *outIncludeName = trimmed.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        return !outIncludeName->empty();
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

    constexpr std::uint64_t kFnv1aOffsetBasis = 1469598103934665603ull;
    constexpr std::uint64_t kFnv1aPrime = 1099511628211ull;

    std::uint64_t HashFnv1a64(std::string_view text) {
        std::uint64_t hash = kFnv1aOffsetBasis;
        for (const char c : text) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            hash *= kFnv1aPrime;
        }
        return hash;
    }

    std::string StripCommentsAndLiterals(std::string_view source) {
        enum class Mode {
            Code,
            LineComment,
            BlockComment,
            StringLiteral,
            CharLiteral,
        };

        Mode mode = Mode::Code;
        std::string out;
        out.reserve(source.size());

        for (std::size_t i = 0; i < source.size(); ++i) {
            const char c = source[i];
            const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

            switch (mode) {
                case Mode::Code:
                    if (c == '/' && next == '/') {
                        mode = Mode::LineComment;
                        out.push_back(' ');
                        ++i;
                        out.push_back(' ');
                    } else if (c == '/' && next == '*') {
                        mode = Mode::BlockComment;
                        out.push_back(' ');
                        ++i;
                        out.push_back(' ');
                    } else if (c == '"') {
                        mode = Mode::StringLiteral;
                        out.push_back(' ');
                    } else if (c == '\'') {
                        mode = Mode::CharLiteral;
                        out.push_back(' ');
                    } else {
                        out.push_back(c);
                    }
                    break;

                case Mode::LineComment:
                    if (c == '\n') {
                        mode = Mode::Code;
                        out.push_back('\n');
                    } else {
                        out.push_back(' ');
                    }
                    break;

                case Mode::BlockComment:
                    if (c == '*' && next == '/') {
                        mode = Mode::Code;
                        out.push_back(' ');
                        ++i;
                        out.push_back(' ');
                    } else {
                        out.push_back(c == '\n' ? '\n' : ' ');
                    }
                    break;

                case Mode::StringLiteral:
                    if (c == '\\' && next != '\0') {
                        out.push_back(' ');
                        ++i;
                        out.push_back(' ');
                    } else if (c == '"') {
                        mode = Mode::Code;
                        out.push_back(' ');
                    } else {
                        out.push_back(c == '\n' ? '\n' : ' ');
                    }
                    break;

                case Mode::CharLiteral:
                    if (c == '\\' && next != '\0') {
                        out.push_back(' ');
                        ++i;
                        out.push_back(' ');
                    } else if (c == '\'') {
                        mode = Mode::Code;
                        out.push_back(' ');
                    } else {
                        out.push_back(c == '\n' ? '\n' : ' ');
                    }
                    break;
            }
        }

        return out;
    }

    bool IsIdentifierCharForScan(const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    bool IsStandaloneKeyword(std::string_view source, std::size_t pos, std::string_view keyword) {
        if (pos + keyword.size() > source.size()) {
            return false;
        }
        if (source.substr(pos, keyword.size()) != keyword) {
            return false;
        }

        const bool hasBefore = (pos > 0);
        if (hasBefore && IsIdentifierCharForScan(source[pos - 1])) {
            return false;
        }
        const std::size_t after = pos + keyword.size();
        if (after < source.size() && IsIdentifierCharForScan(source[after])) {
            return false;
        }
        return true;
    }

    std::string NormalizeAbiSignature(std::string_view text) {
        std::string normalized;
        normalized.reserve(text.size());
        for (const char c : text) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                normalized.push_back(c);
            }
        }
        return normalized;
    }

    std::uint64_t ComputeStateAbiHashFromCppSource(const std::string& source) {
        const std::string sanitized = StripCommentsAndLiterals(source);
        std::string signature;

        std::size_t cursor = 0;
        while (cursor < sanitized.size()) {
            std::size_t keywordPos = std::string::npos;
            std::size_t keywordLen = 0;

            for (std::size_t i = cursor; i < sanitized.size(); ++i) {
                if (IsStandaloneKeyword(sanitized, i, "struct")) {
                    keywordPos = i;
                    keywordLen = 6;
                    break;
                }
                if (IsStandaloneKeyword(sanitized, i, "class")) {
                    keywordPos = i;
                    keywordLen = 5;
                    break;
                }
            }

            if (keywordPos == std::string::npos) {
                break;
            }

            const std::size_t searchFrom = keywordPos + keywordLen;
            const std::size_t bodyStart = sanitized.find('{', searchFrom);
            if (bodyStart == std::string::npos) {
                break;
            }

            const std::size_t nextSemicolon = sanitized.find(';', searchFrom);
            if (nextSemicolon != std::string::npos && nextSemicolon < bodyStart) {
                cursor = nextSemicolon + 1;
                continue;
            }

            int depth = 0;
            std::size_t bodyEnd = std::string::npos;
            for (std::size_t i = bodyStart; i < sanitized.size(); ++i) {
                if (sanitized[i] == '{') {
                    ++depth;
                } else if (sanitized[i] == '}') {
                    --depth;
                    if (depth == 0) {
                        bodyEnd = i;
                        break;
                    }
                }
            }
            if (bodyEnd == std::string::npos) {
                break;
            }

            std::size_t declEnd = sanitized.find(';', bodyEnd + 1);
            if (declEnd == std::string::npos) {
                declEnd = bodyEnd;
            }

            const std::string_view declaration(
                sanitized.data() + keywordPos,
                (declEnd - keywordPos) + 1);
            signature += NormalizeAbiSignature(declaration);
            signature.push_back('\n');
            cursor = declEnd + 1;
        }

        if (signature.empty()) {
            return 0;
        }
        return HashFnv1a64(signature);
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
    networkEnabled_ = ui_ ? ui_->IsNetworkEnabled() : true;
    if (networkEnabled_) {
        if (!InitLanShare()) {
            ui_->AddLogOutput("[Network] LAN workspace sharing is unavailable.");
        }
    } else if (ui_) {
        ui_->AddLogOutput("[Network] LAN workspace sharing is disabled by preference.");
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

    int initialWidth = WINDOW_WIDTH;
    int initialHeight = WINDOW_HEIGHT;
    int initialPosX = 0;
    int initialPosY = 0;
    bool hasInitialPosition = false;

    if (GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor(); primaryMonitor != nullptr) {
        int workX = 0;
        int workY = 0;
        int workWidth = 0;
        int workHeight = 0;
        glfwGetMonitorWorkarea(primaryMonitor, &workX, &workY, &workWidth, &workHeight);
        if (workWidth > 0 && workHeight > 0) {
            initialWidth = workWidth;
            initialHeight = workHeight;
            initialPosX = workX;
            initialPosY = workY;
            hasInitialPosition = true;
        } else if (const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor); mode != nullptr &&
                   mode->width > 0 && mode->height > 0) {
            initialWidth = mode->width;
            initialHeight = mode->height;
            initialPosX = 0;
            initialPosY = 0;
            hasInitialPosition = true;
        }
    }

    window_ = glfwCreateWindow(initialWidth, initialHeight, WINDOW_TITLE, nullptr, nullptr);
    if (!window_) {
        std::cerr << "Window creation failed\n";
        glfwTerminate();
        return false;
    }

    if (hasInitialPosition) {
        glfwSetWindowPos(window_, initialPosX, initialPosY);
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

bool Engine::EnsureWorkspaceGeometry(WorkspaceState* workspace) {
    if (workspace == nullptr) {
        return false;
    }

    DebugLog("EnsureWorkspaceGeometry(name=" + workspace->name +
             ", vao=" + std::to_string(workspace->vao) +
             ", vbo=" + std::to_string(workspace->vbo) + ")");

    if (workspace->vao == 0 || glIsVertexArray(workspace->vao) == GL_FALSE) {
        if (workspace->vao != 0) {
            glDeleteVertexArrays(1, &workspace->vao);
            workspace->vao = 0;
        }
        glGenVertexArrays(1, &workspace->vao);
    }
    if (workspace->vbo == 0 || glIsBuffer(workspace->vbo) == GL_FALSE) {
        if (workspace->vbo != 0) {
            glDeleteBuffers(1, &workspace->vbo);
            workspace->vbo = 0;
        }
        glGenBuffers(1, &workspace->vbo);
    }

    if (workspace->vao == 0 || workspace->vbo == 0) {
        return false;
    }

    glBindVertexArray(workspace->vao);
    glBindBuffer(GL_ARRAY_BUFFER, workspace->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sizeof(FULLSCREEN_QUAD_VERTICES)),
                 FULLSCREEN_QUAD_VERTICES.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    DebugLog("EnsureWorkspaceGeometry OK(name=" + workspace->name +
             ", vao=" + std::to_string(workspace->vao) +
             ", vbo=" + std::to_string(workspace->vbo) + ")");
    return true;
}

void Engine::ReleaseWorkspaceGeometry(WorkspaceState* workspace) {
    if (workspace == nullptr) {
        return;
    }

    DebugLog("ReleaseWorkspaceGeometry(name=" + workspace->name +
             ", vao=" + std::to_string(workspace->vao) +
             ", vbo=" + std::to_string(workspace->vbo) + ")");

    if (workspace->vbo != 0) {
        glDeleteBuffers(1, &workspace->vbo);
        workspace->vbo = 0;
    }
    if (workspace->vao != 0) {
        glDeleteVertexArrays(1, &workspace->vao);
        workspace->vao = 0;
    }
}

bool Engine::EnsureWorkspacePassTargets(WorkspaceState* workspace, const int width, const int height) {
    if (workspace == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    const bool alreadyValid = workspace->passWidth == width &&
                              workspace->passHeight == height &&
                              workspace->passFbos[0] != 0 &&
                              workspace->passFbos[1] != 0 &&
                              workspace->passColorTextures[0] != 0 &&
                              workspace->passColorTextures[1] != 0;
    if (alreadyValid) {
        return true;
    }

    ReleaseWorkspacePassTargets(workspace);

    for (int i = 0; i < 2; ++i) {
        glGenTextures(1, &workspace->passColorTextures[static_cast<std::size_t>(i)]);
        glBindTexture(GL_TEXTURE_2D, workspace->passColorTextures[static_cast<std::size_t>(i)]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &workspace->passFbos[static_cast<std::size_t>(i)]);
        glBindFramebuffer(GL_FRAMEBUFFER, workspace->passFbos[static_cast<std::size_t>(i)]);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               workspace->passColorTextures[static_cast<std::size_t>(i)],
                               0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            ReleaseWorkspacePassTargets(workspace);
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (workspace->gpuQueries[0] == 0 || workspace->gpuQueries[1] == 0) {
        glGenQueries(2, workspace->gpuQueries.data());
    }
    workspace->gpuQueryPending = { false, false };
    workspace->gpuQueryWriteIndex = 0;
    workspace->lastGpuPassTimeMs = 0.0f;
    workspace->passWriteIndex = 0;
    workspace->passWidth = width;
    workspace->passHeight = height;
    return true;
}

void Engine::ReleaseWorkspacePassTargets(WorkspaceState* workspace) {
    if (workspace == nullptr) {
        return;
    }

    for (GLuint& fbo : workspace->passFbos) {
        if (fbo != 0) {
            glDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
    }
    for (GLuint& tex : workspace->passColorTextures) {
        if (tex != 0) {
            glDeleteTextures(1, &tex);
            tex = 0;
        }
    }
    if (workspace->gpuQueries[0] != 0 || workspace->gpuQueries[1] != 0) {
        glDeleteQueries(2, workspace->gpuQueries.data());
        workspace->gpuQueries = { 0, 0 };
    }
    workspace->gpuQueryPending = { false, false };
    workspace->gpuQueryWriteIndex = 0;
    workspace->passWidth = 0;
    workspace->passHeight = 0;
    workspace->passWriteIndex = 0;
}

void Engine::ActivateWorkspaceGeometry(const std::string& workspaceName) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        DebugLog("ActivateWorkspaceGeometry missing workspace '" + workspaceName + "'");
        return;
    }

    if (!EnsureWorkspaceGeometry(&workspaceIt->second)) {
        ctx_.vao = 0;
        ctx_.vbo = 0;
        ui_->AddLogOutput("[Renderer Error] Failed to initialize workspace geometry for '" + workspaceName + "'.");
        return;
    }

    ctx_.vao = workspaceIt->second.vao;
    ctx_.vbo = workspaceIt->second.vbo;
    DebugLog("ActivateWorkspaceGeometry(name=" + workspaceName +
             ", ctx.vao=" + std::to_string(ctx_.vao) +
             ", ctx.vbo=" + std::to_string(ctx_.vbo) +
             ", isVAO=" + std::to_string(ctx_.vao != 0 && glIsVertexArray(ctx_.vao) == GL_TRUE) +
             ", isVBO=" + std::to_string(ctx_.vbo != 0 && glIsBuffer(ctx_.vbo) == GL_TRUE) + ")");
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
    ui_->SetNetworkEnabledChangedCallback([this](const bool enabled) {
        HandleNetworkEnabledChanged(enabled);
    });
    ui_->SetHardResetRuntimeCallback([this]() {
        HardResetActiveWorkspaceState("manual hard reset", true);
    });
    ui_->SetUniformEditCallback([this](const UniformEditCommand& command) {
        HandleUniformEditCommand(command);
    });
    ui_->SetUniformJsonSnapshotCallback([this]() {
        return ActiveWorkspaceUniformStateJson();
    });
    ui_->SetPlaybackCommandCallback([this](const EditorUI::PlaybackCommand& command) {
        HandlePlaybackCommand(command);
    });
    ui_->SetLoadShowcaseWorkspaceCallback([this]() {
        HandleLoadShowcaseWorkspaceRequest();
    });
    ui_->SetPipelineMoveCallback([this](const std::string& workspaceName, int delta) {
        HandlePipelineMoveCommand(workspaceName, delta);
    });
    ui_->SetPipelineEditCallback([this](const EditorUI::PipelineEditCommand& command) {
        HandlePipelineConfigChanged(command);
    });
    ui_->SetPipelineAddPassCallback([this](const std::string& workspaceName) {
        HandlePipelineAddPassCommand(workspaceName);
    });
    ui_->SetPipelineSaveChainCallback([this](const std::string& path) {
        return HandlePipelineSaveChainRequest(path);
    });
    ui_->SetPipelineResetCallback([this]() {
        HandlePipelineResetRequest();
    });
    ui_->SetPipelineOpenFileCallback([this](const std::string& workspaceName, bool openCppFile) {
        HandlePipelineOpenFileRequest(workspaceName, openCppFile);
    });
    ui_->SetPipelineGlobalUniformCallback([this](const EditorUI::PipelineGlobalUniformCommand& command) {
        HandlePipelineGlobalUniformCommand(command);
    });
    ui_->SetPipelineResourceExportCallback([this](const std::string& resourceName, const std::string& targetPath) {
        return ExportPipelineResource(resourceName, targetPath);
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

void Engine::SaveWorkspaceRuntimeState(const std::string& workspaceName) {
    if (workspaceName.empty()) {
        return;
    }

    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    auto& workspace = workspaceIt->second;
    std::ranges::copy(ctx_.state_i, workspace.stateI.begin());
    std::ranges::copy(ctx_.state_f, workspace.stateF.begin());
    std::ranges::copy(ctx_.state_buffer, workspace.stateBuffer.begin());
    workspace.userData = ctx_.userData;
    workspace.stateAbiHash = ctx_.state_abi_hash;
    if (!workspace.arenaStorage.empty()) {
        workspace.arenaOffset = std::min(ctx_.arena_offset, workspace.arenaStorage.size());
    } else {
        workspace.arenaOffset = 0;
    }
}

void Engine::SaveActiveWorkspaceRuntimeState() {
    if (activeWorkspaceName_.empty()) {
        return;
    }

    SaveWorkspaceRuntimeState(activeWorkspaceName_);
    auto workspaceIt = workspaces_.find(activeWorkspaceName_);
    if (workspaceIt == workspaces_.end()) {
        return;
    }
    const auto& workspace = workspaceIt->second;
    DebugLog("SaveActiveWorkspaceRuntimeState(name=" + activeWorkspaceName_ +
             ", ctx.vao=" + std::to_string(ctx_.vao) +
             ", ctx.vbo=" + std::to_string(ctx_.vbo) +
             ", " + FormatStateSlice(workspace.stateI, workspace.stateF) + ")");
}

void Engine::PersistWorkspaceUniformState(const std::string& workspaceName) const {
    if (!workspaceManager_ || workspaceName.empty()) {
        return;
    }

    const auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    const auto& workspace = workspaceIt->second;
    if (workspace.uniformsPath.empty()) {
        return;
    }

    const std::string json = workspace.uniforms.SerializeToJson();
    (void)workspaceManager_->SaveFile(workspace.uniformsPath, json);
}

void Engine::LoadWorkspaceRuntimeState(const std::string& workspaceName) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    auto& workspace = workspaceIt->second;
    std::ranges::copy(workspace.stateI, std::begin(ctx_.state_i));
    std::ranges::copy(workspace.stateF, std::begin(ctx_.state_f));
    std::ranges::copy(workspace.stateBuffer, std::begin(ctx_.state_buffer));
    ctx_.userData = workspace.userData;
    ctx_.state_abi_hash = workspace.stateAbiHash;
    ctx_.arena_base = workspace.arenaStorage.empty() ? nullptr : workspace.arenaStorage.data();
    ctx_.arena_size = workspace.arenaStorage.size();
    ctx_.arena_offset = std::min(workspace.arenaOffset, workspace.arenaStorage.size());
    ctx_.reset_state_requested = false;
    DebugLog("LoadWorkspaceRuntimeState(name=" + workspaceName +
             ", workspace.vao=" + std::to_string(workspace.vao) +
             ", workspace.vbo=" + std::to_string(workspace.vbo) +
             ", " + FormatStateSlice(workspace.stateI, workspace.stateF) + ")");
}

void Engine::ResetWorkspaceRuntimeState(WorkspaceState* workspace, const bool clearStateAbiHash) {
    if (workspace == nullptr) {
        return;
    }

    workspace->stateI.fill(0);
    workspace->stateF.fill(0.0f);
    workspace->stateBuffer.fill(0);
    workspace->userData = nullptr;
    workspace->arenaOffset = 0;
    if (clearStateAbiHash) {
        workspace->stateAbiHash = 0;
    }

    const bool isActiveWorkspace = (!activeWorkspaceName_.empty() && workspace->name == activeWorkspaceName_);
    if (isActiveWorkspace) {
        ctx_.clear_runtime_state();
        if (!clearStateAbiHash) {
            ctx_.state_abi_hash = workspace->stateAbiHash;
        }
        ctx_.arena_base = workspace->arenaStorage.empty() ? nullptr : workspace->arenaStorage.data();
        ctx_.arena_size = workspace->arenaStorage.size();
        ctx_.arena_offset = workspace->arenaOffset;
        ctx_.reset_state_requested = false;
    }
}

void Engine::HardResetActiveWorkspaceState(const std::string& reason, const bool clearStateAbiHash) {
    if (activeWorkspaceName_.empty()) {
        return;
    }

    auto workspaceIt = workspaces_.find(activeWorkspaceName_);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    if (activeProgram_) {
        ShutdownProgramIfInitialized(activeProgram_);
    }

    ResetWorkspaceRuntimeState(&workspaceIt->second, clearStateAbiHash);
    LoadWorkspaceRuntimeState(activeWorkspaceName_);

    if (activeProgram_) {
        InitializeProgramIfNeeded(activeProgram_);
    }
    SaveActiveWorkspaceRuntimeState();
    ui_->AddLogOutput("[Runtime] Active workspace state reset (" + reason + ").");
}

void Engine::HandleUniformEditCommand(const UniformEditCommand& command) {
    if (activeWorkspaceName_.empty()) {
        return;
    }

    auto workspaceIt = workspaces_.find(activeWorkspaceName_);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    if (!workspaceIt->second.uniforms.ApplyEdit(command)) {
        DebugLog("UniformEdit rejected(workspace=" + activeWorkspaceName_ +
                 ", name=" + command.name +
                 ", action=" + std::to_string(static_cast<int>(command.action)) + ")");
        return;
    }
    DebugLog("UniformEdit applied(workspace=" + activeWorkspaceName_ +
             ", name=" + command.name +
             ", action=" + std::to_string(static_cast<int>(command.action)) + ")");
}

std::string Engine::ActiveWorkspaceUniformStateJson() const {
    if (activeWorkspaceName_.empty()) {
        return "{}\n";
    }

    const auto workspaceIt = workspaces_.find(activeWorkspaceName_);
    if (workspaceIt == workspaces_.end()) {
        return "{}\n";
    }

    return workspaceIt->second.uniforms.SerializeToJson();
}

void Engine::NormalizeWorkspacePlaybackRange(WorkspaceState* workspace) {
    if (workspace == nullptr) {
        return;
    }

    if (!std::isfinite(workspace->accumulatedActiveSeconds) || workspace->accumulatedActiveSeconds < 0.0) {
        workspace->accumulatedActiveSeconds = 0.0;
    }
    if (!std::isfinite(workspace->timelineMaxSeconds) || workspace->timelineMaxSeconds < 1.0) {
        workspace->timelineMaxSeconds = 1.0;
    }
    if (!std::isfinite(workspace->loopStartSeconds) || workspace->loopStartSeconds < 0.0) {
        workspace->loopStartSeconds = 0.0;
    }
    if (!std::isfinite(workspace->loopEndSeconds)) {
        workspace->loopEndSeconds = workspace->loopStartSeconds + 10.0;
    }
    if (workspace->loopEndSeconds <= workspace->loopStartSeconds) {
        workspace->loopEndSeconds = workspace->loopStartSeconds + 0.01;
    }

    if (workspace->loopEnabled) {
        const double loopRange = workspace->loopEndSeconds - workspace->loopStartSeconds;
        if (loopRange > 0.0) {
            if (workspace->accumulatedActiveSeconds < workspace->loopStartSeconds) {
                workspace->accumulatedActiveSeconds = workspace->loopStartSeconds;
            } else if (workspace->accumulatedActiveSeconds >= workspace->loopEndSeconds) {
                double offset = std::fmod(workspace->accumulatedActiveSeconds - workspace->loopStartSeconds, loopRange);
                if (offset < 0.0) {
                    offset += loopRange;
                }
                workspace->accumulatedActiveSeconds = workspace->loopStartSeconds + offset;
            }
        }
    }

    if (workspace->loopEndSeconds > workspace->timelineMaxSeconds) {
        workspace->timelineMaxSeconds = workspace->loopEndSeconds;
    }
    if (!workspace->loopEnabled && workspace->accumulatedActiveSeconds > workspace->timelineMaxSeconds) {
        workspace->timelineMaxSeconds = workspace->accumulatedActiveSeconds;
    }
}

void Engine::AdvanceWorkspacePlayback(WorkspaceState* workspace, const double nowSeconds) {
    if (workspace == nullptr) {
        return;
    }

    if (workspace->activeClockStartSeconds < 0.0) {
        workspace->activeClockStartSeconds = nowSeconds;
        NormalizeWorkspacePlaybackRange(workspace);
        return;
    }

    const double deltaSeconds = std::max(0.0, nowSeconds - workspace->activeClockStartSeconds);
    workspace->activeClockStartSeconds = nowSeconds;

    if (!workspace->playbackPaused && std::isfinite(workspace->playbackSpeed) && workspace->playbackSpeed > 0.0f) {
        workspace->accumulatedActiveSeconds += deltaSeconds * static_cast<double>(workspace->playbackSpeed);
    }

    NormalizeWorkspacePlaybackRange(workspace);
}

EditorUI::PlaybackState Engine::BuildPlaybackStateForWorkspace(const std::string& workspaceName) const {
    EditorUI::PlaybackState state;

    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return state;
    }

    const auto& workspace = workspaceIt->second;
    state.currentTimeSeconds = static_cast<float>(std::max(0.0, workspace.accumulatedActiveSeconds));
    state.timelineMaxSeconds = static_cast<float>(std::max(1.0, workspace.timelineMaxSeconds));
    state.paused = workspace.playbackPaused;
    state.speed = workspace.playbackSpeed;
    state.loopEnabled = workspace.loopEnabled;
    state.loopStartSeconds = static_cast<float>(std::max(0.0, workspace.loopStartSeconds));
    state.loopEndSeconds = static_cast<float>(std::max(workspace.loopStartSeconds + 0.01, workspace.loopEndSeconds));
    return state;
}

void Engine::HandlePlaybackCommand(const EditorUI::PlaybackCommand& command) {
    if (activeWorkspaceName_.empty()) {
        return;
    }

    auto workspaceIt = workspaces_.find(activeWorkspaceName_);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    const double nowSeconds = glfwGetTime();
    auto& workspace = workspaceIt->second;
    AdvanceWorkspacePlayback(&workspace, nowSeconds);

    switch (command.type) {
        case EditorUI::PlaybackCommandType::TogglePause:
            workspace.playbackPaused = !workspace.playbackPaused;
            break;

        case EditorUI::PlaybackCommandType::Rewind:
            workspace.accumulatedActiveSeconds = 0.0;
            break;

        case EditorUI::PlaybackCommandType::SetTime:
            workspace.accumulatedActiveSeconds = std::max(0.0, static_cast<double>(command.value));
            break;

        case EditorUI::PlaybackCommandType::SetSpeed: {
            constexpr std::array<float, 4> allowedSpeeds = { 0.25f, 0.5f, 1.0f, 2.0f };
            float selected = allowedSpeeds.front();
            float smallestDistance = std::abs(command.value - selected);
            for (const float candidate : allowedSpeeds) {
                const float distance = std::abs(command.value - candidate);
                if (distance < smallestDistance) {
                    selected = candidate;
                    smallestDistance = distance;
                }
            }
            workspace.playbackSpeed = selected;
            break;
        }

        case EditorUI::PlaybackCommandType::SetLoopEnabled:
            workspace.loopEnabled = command.enabled;
            break;

        case EditorUI::PlaybackCommandType::SetLoopStart:
            workspace.loopStartSeconds = std::max(0.0, static_cast<double>(command.value));
            break;

        case EditorUI::PlaybackCommandType::SetLoopEnd:
            workspace.loopEndSeconds = std::max(0.0, static_cast<double>(command.value));
            break;

        case EditorUI::PlaybackCommandType::SetTimelineMax:
            workspace.timelineMaxSeconds = std::max(1.0, static_cast<double>(command.value));
            break;
    }

    NormalizeWorkspacePlaybackRange(&workspace);
    ui_->SetPlaybackState(BuildPlaybackStateForWorkspace(activeWorkspaceName_));
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

bool Engine::BuildCompileSourceForWorkspace(const std::string& workspaceName,
                                            std::string* outSource,
                                            std::vector<UniformDescriptor>* outUniformDescriptors,
                                            std::vector<std::string>* outSamplerUniformNames,
                                            std::vector<std::string>* outStorageBufferNames,
                                            std::vector<std::string>* outShaderDependencies,
                                            std::string* outError) const {
    if (outSource == nullptr ||
        outUniformDescriptors == nullptr ||
        outSamplerUniformNames == nullptr ||
        outStorageBufferNames == nullptr ||
        outShaderDependencies == nullptr ||
        outError == nullptr) {
        return false;
    }

    outSource->clear();
    outUniformDescriptors->clear();
    outSamplerUniformNames->clear();
    outStorageBufferNames->clear();
    outShaderDependencies->clear();
    outError->clear();

    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return false;
    }

    const auto& workspace = workspaceIt->second;
    std::unordered_map<std::string, std::string> shaderContentsByKey;
    std::unordered_map<std::string, std::string> shaderPathByKey;
    std::unordered_map<std::string, std::string> basenameToKey;
    std::unordered_set<std::string> ambiguousBasenames;

    std::error_code fsError;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             WORKSPACE_DIR,
             std::filesystem::directory_options::skip_permission_denied,
             fsError)) {
        if (fsError || !entry.is_regular_file(fsError)) {
            continue;
        }
        if (entry.path().extension() != ".glsl") {
            continue;
        }
        const auto content = workspaceManager_ ? workspaceManager_->ReadFile(entry.path().string()) : std::nullopt;
        if (!content.has_value()) {
            continue;
        }

        std::error_code relativeError;
        std::filesystem::path relativePath = std::filesystem::relative(entry.path(), WORKSPACE_DIR, relativeError);
        if (relativeError) {
            relativePath = entry.path().filename();
        }
        const std::string key = NormalizePathString(relativePath);
        const std::string fullPath = NormalizePathString(entry.path());
        shaderContentsByKey[key] = *content;
        shaderPathByKey[key] = fullPath;

        const std::string basename = entry.path().filename().string();
        if (ambiguousBasenames.contains(basename)) {
            continue;
        }
        if (const auto existing = basenameToKey.find(basename); existing != basenameToKey.end() && existing->second != key) {
            basenameToKey.erase(existing);
            ambiguousBasenames.insert(basename);
        } else {
            basenameToKey[basename] = key;
        }
    }

    std::error_code workspaceRelativeError;
    std::filesystem::path workspaceShaderRelative = std::filesystem::relative(workspace.shaderPath,
                                                                              WORKSPACE_DIR,
                                                                              workspaceRelativeError);
    if (workspaceRelativeError) {
        workspaceShaderRelative = std::filesystem::path(workspaceName) / "shader.glsl";
    }
    const std::string rootShaderKey = NormalizePathString(workspaceShaderRelative);
    shaderContentsByKey[rootShaderKey] = workspace.shaderSource;
    shaderPathByKey[rootShaderKey] = NormalizePathString(workspace.shaderPath);
    basenameToKey[std::filesystem::path(workspace.shaderPath).filename().string()] = rootShaderKey;

    std::unordered_set<std::string> includeStack;
    std::unordered_set<std::string> includedKeys;
    std::function<bool(const std::string&, std::string*)> expandShaderKey;
    expandShaderKey = [&](const std::string& key, std::string* outExpanded) -> bool {
        if (outExpanded == nullptr) {
            return false;
        }
        const auto sourceIt = shaderContentsByKey.find(key);
        if (sourceIt == shaderContentsByKey.end()) {
            *outError = "Missing include file: " + key;
            return false;
        }
        if (includeStack.contains(key)) {
            *outError = "Cyclic shader include detected at '" + key + "'";
            return false;
        }

        includeStack.insert(key);
        const std::string sourceText = sourceIt->second;
        std::size_t cursor = 0;
        while (cursor <= sourceText.size()) {
            const std::size_t lineEnd = sourceText.find('\n', cursor);
            const std::size_t lineLength = (lineEnd == std::string::npos) ? (sourceText.size() - cursor)
                                                                           : (lineEnd - cursor);
            const std::string line = sourceText.substr(cursor, lineLength);

            std::string includeName;
            if (ParseIncludeDirective(line, &includeName)) {
                std::string resolvedKey;
                const std::filesystem::path includeRelative =
                    std::filesystem::path(key).parent_path() / std::filesystem::path(includeName);
                const std::string relativeCandidate = NormalizePathString(includeRelative);
                if (shaderContentsByKey.contains(relativeCandidate)) {
                    resolvedKey = relativeCandidate;
                } else if (shaderContentsByKey.contains(includeName)) {
                    resolvedKey = includeName;
                } else if (const auto basenameIt = basenameToKey.find(includeName); basenameIt != basenameToKey.end()) {
                    resolvedKey = basenameIt->second;
                }

                if (resolvedKey.empty()) {
                    *outError = "Failed to resolve shader include '" + includeName + "' from '" + key + "'";
                    includeStack.erase(key);
                    return false;
                }

                std::string includedExpanded;
                if (!expandShaderKey(resolvedKey, &includedExpanded)) {
                    includeStack.erase(key);
                    return false;
                }
                includedKeys.insert(resolvedKey);
                *outExpanded += includedExpanded;
            } else {
                *outExpanded += line;
                *outExpanded += '\n';
            }

            if (lineEnd == std::string::npos) {
                break;
            }
            cursor = lineEnd + 1;
        }

        includeStack.erase(key);
        return true;
    };

    std::string expandedShaderSource;
    if (!expandShaderKey(rootShaderKey, &expandedShaderSource)) {
        std::ostringstream generatedSource;
        generatedSource << "#error \"" << escapeForCStringLiteral(*outError) << "\"\n";
        generatedSource << workspace.cppSource;
        *outSource = generatedSource.str();
        return true;
    }

    for (const auto& includeKey : includedKeys) {
        if (const auto pathIt = shaderPathByKey.find(includeKey); pathIt != shaderPathByKey.end()) {
            if (pathIt->second != NormalizePathString(workspace.shaderPath)) {
                outShaderDependencies->push_back(pathIt->second);
            }
        }
    }
    std::ranges::sort(*outShaderDependencies);
    outShaderDependencies->erase(std::unique(outShaderDependencies->begin(), outShaderDependencies->end()),
                                 outShaderDependencies->end());

    ShaderSections sections;
    if (!parseShaderSections(expandedShaderSource, &sections)) {
        *outError = "shader.glsl must define '#type vertex'+'#type fragment' and/or '#type compute' sections";
        std::ostringstream generatedSource;
        generatedSource << "#error \"" << escapeForCStringLiteral(*outError) << "\"\n";
        generatedSource << workspace.cppSource;
        *outSource = generatedSource.str();
        return true;
    }

    const bool hasGraphics = !sections.vertex.empty() || !sections.fragment.empty();
    if (hasGraphics && (sections.vertex.empty() || sections.fragment.empty())) {
        *outError = "Graphics shader path requires both '#type vertex' and '#type fragment' sections";
        std::ostringstream generatedSource;
        generatedSource << "#error \"" << escapeForCStringLiteral(*outError) << "\"\n";
        generatedSource << workspace.cppSource;
        *outSource = generatedSource.str();
        return true;
    }

    *outUniformDescriptors = ParseUniformDescriptors(expandedShaderSource);
    *outSamplerUniformNames = ParseSamplerUniformNames(expandedShaderSource);
    *outStorageBufferNames = ParseStorageBufferNames(expandedShaderSource);

    // Inject parsed shader sources and hash into the generated C++ translation unit.
    const uint32_t shaderHash = static_cast<uint32_t>(std::hash<std::string>{}(expandedShaderSource));
    const std::uint64_t stateAbiHash = ComputeStateAbiHashFromCppSource(workspace.cppSource);
    std::ostringstream generatedSource;
    generatedSource << "static const unsigned int jitgl_workspace_shader_hash = " << shaderHash << "u;\n";
    generatedSource << "static const unsigned long long jitgl_workspace_state_abi_hash = "
                    << stateAbiHash << "ull;\n";
    generatedSource << "static const bool jitgl_workspace_has_graphics = "
                    << (hasGraphics ? "true" : "false") << ";\n";
    generatedSource << "static const bool jitgl_workspace_has_compute = "
                    << (!sections.compute.empty() ? "true" : "false") << ";\n";
    generatedSource << "static const char* jitgl_workspace_vertex_shader_source = \""
                    << escapeForCStringLiteral(sections.vertex) << "\";\n";
    generatedSource << "static const char* jitgl_workspace_fragment_shader_source = \""
                    << escapeForCStringLiteral(sections.fragment) << "\";\n";
    generatedSource << "static const char* jitgl_workspace_compute_shader_source = \""
                    << escapeForCStringLiteral(sections.compute) << "\";\n";
    generatedSource << "#define JIT_WORKSPACE_VERTEX_SHADER jitgl_workspace_vertex_shader_source\n";
    generatedSource << "#define JIT_WORKSPACE_FRAGMENT_SHADER jitgl_workspace_fragment_shader_source\n";
    generatedSource << "#define JIT_WORKSPACE_COMPUTE_SHADER jitgl_workspace_compute_shader_source\n";
    generatedSource << "#define JIT_WORKSPACE_SHADER_HASH jitgl_workspace_shader_hash\n";
    generatedSource << "#define JIT_WORKSPACE_STATE_ABI_HASH jitgl_workspace_state_abi_hash\n";
    generatedSource << "#define JIT_WORKSPACE_HAS_GRAPHICS jitgl_workspace_has_graphics\n";
    generatedSource << "#define JIT_WORKSPACE_HAS_COMPUTE jitgl_workspace_has_compute\n";
    generatedSource << workspace.cppSource;

    *outSource = generatedSource.str();
    return true;
}

void Engine::QueueCompileForWorkspace(const std::string& workspaceName, double nowSeconds, bool immediate) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    std::string generatedSource;
    std::vector<UniformDescriptor> uniformDescriptors;
    std::vector<std::string> samplerUniformNames;
    std::vector<std::string> storageBufferNames;
    std::vector<std::string> shaderDependencies;
    std::string buildError;
    if (!BuildCompileSourceForWorkspace(workspaceName,
                                        &generatedSource,
                                        &uniformDescriptors,
                                        &samplerUniformNames,
                                        &storageBufferNames,
                                        &shaderDependencies,
                                        &buildError)) {
        return;
    }

    const std::uint64_t stateAbiHash = ComputeStateAbiHashFromCppSource(workspaceIt->second.cppSource);
    if (!buildError.empty()) {
        ui_->AddLogOutput("[Shader Include] " + buildError);
    }
    DebugLog("QueueCompileForWorkspace(name=" + workspaceName +
             ", immediate=" + std::to_string(immediate) +
             ", uniforms=" + std::to_string(uniformDescriptors.size()) + ")");
    QueueCompile(workspaceName,
                 generatedSource,
                 stateAbiHash,
                 std::move(uniformDescriptors),
                 std::move(samplerUniformNames),
                 std::move(storageBufferNames),
                 std::move(shaderDependencies),
                 nowSeconds,
                 immediate);
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
    workspace.uniformsPath = descriptor.uniformsPath;
    workspace.consoleLogPath = descriptor.consoleLogPath;
    workspace.engineLogPath = descriptor.engineLogPath;
    workspace.cppSource = std::move(*cppSource);
    workspace.shaderSource = std::move(*shaderSource);
    workspace.arenaStorage.resize(kWorkspaceArenaBytes, 0);
    workspace.arenaOffset = 0;
    workspace.outputTextureName = workspace.name;
    if (auto uniformsJson = workspaceManager_->ReadFile(descriptor.uniformsPath); uniformsJson.has_value()) {
        (void)workspace.uniforms.LoadFromJson(*uniformsJson);
    }
    if (!EnsureWorkspaceGeometry(&workspace)) {
        return false;
    }
    NormalizeWorkspacePlaybackRange(&workspace);

    workspaces_[workspace.name] = workspace;
    DebugLog("RegisterWorkspace(name=" + workspace.name +
             ", vao=" + std::to_string(workspace.vao) +
             ", vbo=" + std::to_string(workspace.vbo) +
             ", cpp=" + workspace.cppPath +
             ", shader=" + workspace.shaderPath + ")");
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

    // Capture only the fields needed after the workspaces_.erase() below — a full
    // WorkspaceState copy would deep-clone the per-workspace 1 MB arena storage.
    const std::string workspaceCppPath = workspaceIt->second.cppPath;
    const std::string workspaceShaderPath = workspaceIt->second.shaderPath;
    const unsigned int workspaceVao = workspaceIt->second.vao;
    const unsigned int workspaceVbo = workspaceIt->second.vbo;
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

    ReleaseWorkspaceGeometry(&workspaceIt->second);
    ReleaseWorkspacePassTargets(&workspaceIt->second);
    if (ctx_.vao == workspaceVao) {
        ctx_.vao = 0;
    }
    if (ctx_.vbo == workspaceVbo) {
        ctx_.vbo = 0;
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
    fileToWorkspace_.erase(workspaceCppPath);
    fileToWorkspace_.erase(workspaceShaderPath);
    latestSources_.erase(workspaceName);
    latestSourceHashes_.erase(workspaceName);
    latestUniformDescriptors_.erase(workspaceName);
    latestSamplerUniformNames_.erase(workspaceName);
    latestStorageBufferNames_.erase(workspaceName);
    latestShaderDependencies_.erase(workspaceName);
    latestStateAbiHashes_.erase(workspaceName);
    pendingCompilesAt_.erase(workspaceName);
    compileFailures_.erase(workspaceName);
    compileRetryAfter_.erase(workspaceName);
    recentErrors_.erase(workspaceName);

    ignoreWatcherUntil_.erase(workspaceCppPath);
    ignoreWatcherUntil_.erase(workspaceShaderPath);

    std::erase(workspaceOrder_, workspaceName);
    pipelineConnections_.erase(workspaceName);
    for (auto& [targetWorkspace, perInput] : pipelineConnections_) {
        (void)targetWorkspace;
        std::erase_if(perInput,
                      [&](const auto& entry) {
                          return entry.second.sourceWorkspace == workspaceName;
                      });
    }
    std::erase_if(pipelineConnections_,
                  [](const auto& entry) {
                      return entry.second.empty();
                  });
    std::erase_if(pipelineGlobalUniforms_,
                  [](const EditorUI::PipelineGlobalUniformView& uniform) {
                      return uniform.name.empty();
                  });

    {
        std::scoped_lock lock(pendingMutex_);
        std::queue<std::string> filteredReloads;
        while (!pendingReloads_.empty()) {
            std::string path = std::move(pendingReloads_.front());
            pendingReloads_.pop();
            if (path == workspaceCppPath || path == workspaceShaderPath) {
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
        DebugLog("SwitchToWorkspace missing '" + workspaceName + "'");
        return;
    }

    DebugLog("SwitchToWorkspace begin(from='" + activeWorkspaceName_ + "', to='" + workspaceName +
             "', focusCpp=" + std::to_string(focusCppDocument) + ")");
    const double nowSeconds = glfwGetTime();
    if (activeWorkspaceName_ != workspaceName) {
        if (auto previousWorkspaceIt = workspaces_.find(activeWorkspaceName_);
            previousWorkspaceIt != workspaces_.end()) {
            auto& previousWorkspace = previousWorkspaceIt->second;
            AdvanceWorkspacePlayback(&previousWorkspace, nowSeconds);
            previousWorkspace.activeClockStartSeconds = -1.0;
        }

        SaveActiveWorkspaceRuntimeState();
        PersistWorkspaceUniformState(activeWorkspaceName_);
        activeProgram_.reset();
        activeWorkspaceName_ = workspaceName;
        LoadWorkspaceRuntimeState(workspaceName);
        ActivateWorkspaceGeometry(workspaceName);
        workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            return;
        }
        ui_->SetActiveWorkspace(workspaceName);
        ui_->SetUniformValues(workspaceIt->second.uniforms.Values());
    }
    workspaceIt->second.activeClockStartSeconds = nowSeconds;

    if (focusCppDocument) {
        activeFilePath_ = workspaceIt->second.cppPath;
        ui_->SetActiveDocument(activeFilePath_);
    }

    if (const bool requiresRecompile = workspaceDirty_[workspaceName]; requiresRecompile) {
        DebugLog("SwitchToWorkspace dirty workspace '" + workspaceName + "', forcing compile");
        ActivateProgramForWorkspace(workspaceName);
        QueueCompileForWorkspace(workspaceName, glfwGetTime(), true);
        ui_->SetPlaybackState(BuildPlaybackStateForWorkspace(workspaceName));
        SaveActiveWorkspaceRuntimeState();
        DebugLog("SwitchToWorkspace end(dirty, activeProgram=" +
                 std::to_string(activeProgram_ != nullptr) + ")");
        return;
    }

    if (!ActivateProgramForWorkspace(workspaceName)) {
        DebugLog("SwitchToWorkspace no compiled program for '" + workspaceName + "', queueing compile");
        QueueCompileForWorkspace(workspaceName, glfwGetTime(), true);
    }
    ui_->SetPlaybackState(BuildPlaybackStateForWorkspace(workspaceName));
    SaveActiveWorkspaceRuntimeState();
    DebugLog("SwitchToWorkspace end(activeProgram=" + std::to_string(activeProgram_ != nullptr) +
             ", ctx.vao=" + std::to_string(ctx_.vao) +
             ", ctx.vbo=" + std::to_string(ctx_.vbo) +
             ", ctx.state_i0=" + std::to_string(ctx_.state_i[0]) + ")");
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

    bool switchedByShowcaseLoad = false;
    if (ui_->ShouldLoadShowcaseWorkspaceOnStartup()) {
        switchedByShowcaseLoad = LoadShowcaseWorkspaceFromAssets(true);
        if (!switchedByShowcaseLoad) {
            ui_->AddLogOutput("[Workspace] Failed to load startup showcase workspace.");
        }
    }

    if (workspaceOrder_.empty()) {
        ui_->AddLogOutput("[Workspace Error] No workspace could be loaded.");
        return false;
    }

    SyncWorkspaceUiState();
    if (!switchedByShowcaseLoad) {
        SwitchToWorkspace(workspaceOrder_.front(), true);
    }

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
        const std::filesystem::path filePath(filepath);
        const std::string normalizedPath = NormalizePathString(filePath);
        const bool isGlslFile = (filePath.extension() == ".glsl");
        auto ignoredIt = ignoreWatcherUntil_.find(filepath);
        if (ignoredIt != ignoreWatcherUntil_.end() && nowSeconds < ignoredIt->second) {
            continue;
        }

        const auto content = workspaceManager_->ReadFile(filepath);
        if (!content.has_value()) {
            if (isGlslFile) {
                bool triggered = false;
                for (auto& [name, workspace] : workspaces_) {
                    if (std::ranges::find(workspace.shaderDependencies, normalizedPath) ==
                        workspace.shaderDependencies.end()) {
                        continue;
                    }
                    QueueCompileForWorkspace(name, nowSeconds, true);
                    triggered = true;
                }
                if (triggered) {
                    dependencyFlashUntil_[normalizedPath] = nowSeconds + 1.5;
                    ui_->AddLogOutput("[Watcher] Shared shader dependency changed (missing/unreadable): " + filepath);
                }
            }
            ui_->AddLogOutput("[Watcher] Failed to read changed file: " + filepath);
            continue;
        }

        const std::string workspaceName = WorkspaceForPath(filepath);
        if (workspaceName.empty()) {
            if (isGlslFile) {
                bool triggered = false;
                for (auto& [name, workspace] : workspaces_) {
                    if (std::ranges::find(workspace.shaderDependencies, normalizedPath) ==
                        workspace.shaderDependencies.end()) {
                        continue;
                    }
                    QueueCompileForWorkspace(name, nowSeconds, true);
                    triggered = true;
                }
                if (triggered) {
                    dependencyFlashUntil_[normalizedPath] = nowSeconds + 1.5;
                    ui_->AddLogOutput("[Watcher] Shared shader dependency changed: " + filepath);
                }
            }
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
        QueueCompileForWorkspace(workspaceName, nowSeconds, true);
        if (isGlslFile) {
            dependencyFlashUntil_[normalizedPath] = nowSeconds + 1.5;
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

void Engine::QueueCompile(const std::string& workspaceName,
                          const std::string& source,
                          const std::uint64_t stateAbiHash,
                          std::vector<UniformDescriptor> uniformDescriptors,
                          std::vector<std::string> samplerUniformNames,
                          std::vector<std::string> storageBufferNames,
                          std::vector<std::string> shaderDependencies,
                          double nowSeconds,
                          bool immediate) {
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
    latestSourceHashes_[workspaceName] = sourceHash;
    latestUniformDescriptors_[workspaceName] = std::move(uniformDescriptors);
    latestSamplerUniformNames_[workspaceName] = std::move(samplerUniformNames);
    latestStorageBufferNames_[workspaceName] = std::move(storageBufferNames);
    latestShaderDependencies_[workspaceName] = std::move(shaderDependencies);
    latestStateAbiHashes_[workspaceName] = stateAbiHash;
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
        auto uniformDescriptorsIt = latestUniformDescriptors_.find(workspaceName);
        if (uniformDescriptorsIt == latestUniformDescriptors_.end()) {
            continue;
        }
        auto samplerUniformsIt = latestSamplerUniformNames_.find(workspaceName);
        if (samplerUniformsIt == latestSamplerUniformNames_.end()) {
            continue;
        }
        auto storageBuffersIt = latestStorageBufferNames_.find(workspaceName);
        if (storageBuffersIt == latestStorageBufferNames_.end()) {
            continue;
        }
        auto shaderDependenciesIt = latestShaderDependencies_.find(workspaceName);
        if (shaderDependenciesIt == latestShaderDependencies_.end()) {
            continue;
        }
        auto stateAbiHashIt = latestStateAbiHashes_.find(workspaceName);
        if (stateAbiHashIt == latestStateAbiHashes_.end()) {
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
            sourceHash,
            stateAbiHashIt->second,
            uniformDescriptorsIt->second,
            samplerUniformsIt->second,
            storageBuffersIt->second,
            shaderDependenciesIt->second
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
            compileResults_.push(CompileResult{
                job.workspaceName,
                std::move(program),
                job.sourceHash,
                job.stateAbiHash,
                std::move(job.uniformDescriptors),
                std::move(job.samplerUniformNames),
                std::move(job.storageBufferNames),
                std::move(job.shaderDependencies)
            });
        }
    }
}

void Engine::InitializeProgramIfNeeded(const std::shared_ptr<JitProgram>& program) {
    if (!program || program->initialized) {
        return;
    }

    DebugLog("InitializeProgramIfNeeded(program_ptr=" +
             std::to_string(reinterpret_cast<std::uintptr_t>(program.get())) +
             ", ctx.state_i0(before)=" + std::to_string(ctx_.state_i[0]) +
             ", ctx.vao=" + std::to_string(ctx_.vao) +
             ", ctx.vbo=" + std::to_string(ctx_.vbo) + ")");

    if (program->functions.init) {
        program->functions.init(&ctx_);
    }
    program->initialized = true;
    DebugLog("InitializeProgramIfNeeded done(program_ptr=" +
             std::to_string(reinterpret_cast<std::uintptr_t>(program.get())) +
             ", ctx.state_i0(after)=" + std::to_string(ctx_.state_i[0]) +
             ", glIsProgram=" + std::to_string(ctx_.state_i[0] != 0 &&
                                               glIsProgram(static_cast<GLuint>(ctx_.state_i[0])) == GL_TRUE) + ")");
}

void Engine::ShutdownProgramIfInitialized(const std::shared_ptr<JitProgram>& program) {
    if (!program || !program->initialized) {
        return;
    }

    DebugLog("ShutdownProgramIfInitialized(program_ptr=" +
             std::to_string(reinterpret_cast<std::uintptr_t>(program.get())) +
             ", ctx.state_i0(before)=" + std::to_string(ctx_.state_i[0]) + ")");

    if (program->functions.shutdown) {
        program->functions.shutdown(&ctx_);
    }
    program->initialized = false;
    DebugLog("ShutdownProgramIfInitialized done(program_ptr=" +
             std::to_string(reinterpret_cast<std::uintptr_t>(program.get())) +
             ", ctx.state_i0(after)=" + std::to_string(ctx_.state_i[0]) + ")");
}

bool Engine::ActivateProgramForWorkspace(const std::string& workspaceName) {
    auto it = compiledPrograms_.find(workspaceName);
    if (it == compiledPrograms_.end()) {
        DebugLog("ActivateProgramForWorkspace missing compiled program for '" + workspaceName + "'");
        return false;
    }

    const auto& nextProgram = it->second;
    activeProgram_ = nextProgram;
    InitializeProgramIfNeeded(activeProgram_);

    DebugLog("ActivateProgramForWorkspace(name=" + workspaceName +
             ", program_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(nextProgram.get())) +
             ", initialized=" + std::to_string(nextProgram != nullptr && nextProgram->initialized) +
             ", ctx.state_i0=" + std::to_string(ctx_.state_i[0]) +
             ", glIsProgram=" + std::to_string(ctx_.state_i[0] != 0 &&
                                               glIsProgram(static_cast<GLuint>(ctx_.state_i[0])) == GL_TRUE) + ")");
    ui_->AddLogOutput("[Runtime] Active workspace switched to " + workspaceName);
    return true;
}

bool Engine::ExportActiveWorkspace(const std::string& targetPath) const {
    if (activeWorkspaceName_.empty()) return false;
    PersistWorkspaceUniformState(activeWorkspaceName_);
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

bool Engine::LoadShowcaseWorkspaceFromAssets(const bool focusWorkspace) {
    if (!workspaceManager_ || !ui_) {
        return false;
    }

    const auto sceneSource = LoadTextFileWithFallback(SHOWCASE_SCENE_ASSET_PATH, SHOWCASE_SCENE_ASSET_FALLBACK_PATH);
    const auto shaderSource = LoadTextFileWithFallback(SHOWCASE_SHADER_ASSET_PATH, SHOWCASE_SHADER_ASSET_FALLBACK_PATH);
    const auto uniformsSource = LoadTextFileWithFallback(SHOWCASE_UNIFORMS_ASSET_PATH,
                                                         SHOWCASE_UNIFORMS_ASSET_FALLBACK_PATH);
    if (!sceneSource.has_value() || !shaderSource.has_value()) {
        ui_->AddLogOutput("[Workspace Error] Showcase assets are missing. Expected assets/showcase_scene.cpp and assets/showcase_shader.glsl.");
        return false;
    }

    auto descriptor = workspaceManager_->CreateWorkspace(SHOWCASE_WORKSPACE_NAME);
    if (!descriptor.has_value()) {
        ui_->AddLogOutput("[Workspace Error] Failed to create or load showcase workspace directory.");
        return false;
    }

    const std::string uniformJson = uniformsSource.value_or("{}\n");
    if (!workspaceManager_->SaveFile(descriptor->cppPath, *sceneSource) ||
        !workspaceManager_->SaveFile(descriptor->shaderPath, *shaderSource) ||
        !workspaceManager_->SaveFile(descriptor->uniformsPath, uniformJson)) {
        ui_->AddLogOutput("[Workspace Error] Failed to write showcase workspace files.");
        return false;
    }

    const double nowSeconds = glfwGetTime();
    ignoreWatcherUntil_[descriptor->cppPath] = nowSeconds + WATCHER_IGNORE_SECONDS;
    ignoreWatcherUntil_[descriptor->shaderPath] = nowSeconds + WATCHER_IGNORE_SECONDS;
    ignoreWatcherUntil_[descriptor->uniformsPath] = nowSeconds + WATCHER_IGNORE_SECONDS;

    auto workspaceIt = workspaces_.find(descriptor->name);
    if (workspaceIt == workspaces_.end()) {
        if (!RegisterWorkspace(*descriptor)) {
            ui_->AddLogOutput("[Workspace Error] Showcase workspace files were written but could not be registered.");
            return false;
        }
        workspaceIt = workspaces_.find(descriptor->name);
        if (workspaceIt == workspaces_.end()) {
            return false;
        }
    } else {
        auto& workspace = workspaceIt->second;
        workspace.cppSource = *sceneSource;
        workspace.shaderSource = *shaderSource;
        workspace.uniforms = UniformRegistry{};
        (void)workspace.uniforms.LoadFromJson(uniformJson);

        ui_->UpdateDocumentContent(workspace.cppPath, workspace.cppSource);
        ui_->UpdateDocumentContent(workspace.shaderPath, workspace.shaderSource);
        ui_->SetWorkspaceOutputHistory(workspace.name,
                                       workspaceManager_->LoadWorkspaceConsoleLog(workspace.name),
                                       workspaceManager_->LoadWorkspaceEngineLog(workspace.name));
    }

    workspaceDirty_[descriptor->name] = true;
    compileFailures_.erase(descriptor->name);
    compileRetryAfter_.erase(descriptor->name);
    recentErrors_.erase(descriptor->name);
    latestSources_.erase(descriptor->name);
    latestUniformDescriptors_.erase(descriptor->name);
    latestStorageBufferNames_.erase(descriptor->name);
    pendingCompilesAt_.erase(descriptor->name);

    SyncWorkspaceUiState();
    if (focusWorkspace) {
        SwitchToWorkspace(descriptor->name, true);
    }
    return true;
}

void Engine::HandleLoadShowcaseWorkspaceRequest() {
    if (!workspaceManager_) {
        ui_->AddLogOutput("[Workspace Error] Cannot load showcase workspace before workspace manager is initialized.");
        return;
    }

    if (!LoadShowcaseWorkspaceFromAssets(true)) {
        ui_->AddLogOutput("[Workspace Error] Failed to load showcase workspace from assets.");
        return;
    }

    ui_->AddLogOutput("[Workspace] Showcase workspace loaded from assets.");
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

    if (!networkEnabled_) {
        EditorUI::NetworkDiagnostics diagnostics;
        diagnostics.lastError = "LAN networking is disabled in preferences.";
        ui_->SetNetworkDiagnostics(std::move(diagnostics));
        ui_->SetNetworkPeers({});
        return;
    }

    if (!lanShare_) {
        EditorUI::NetworkDiagnostics diagnostics;
        diagnostics.lastError = "LAN sharing service was not initialized.";
        ui_->SetNetworkDiagnostics(std::move(diagnostics));
        ui_->SetNetworkPeers({});
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

void Engine::HandleNetworkEnabledChanged(const bool enabled) {
    if (networkEnabled_ == enabled) {
        return;
    }

    networkEnabled_ = enabled;
    pendingLanOffersById_.clear();
    if (ui_) {
        ui_->SetNetworkPeers({});
    }

    if (!networkEnabled_) {
        if (lanShare_) {
            lanShare_->Stop();
            lanShare_.reset();
        }
        if (ui_) {
            ui_->AddLogOutput("[Network] LAN workspace sharing disabled.");
        }
        UpdateLanShareUiState();
        return;
    }

    if (!InitLanShare()) {
        if (ui_) {
            ui_->AddLogOutput("[Network] LAN workspace sharing is unavailable.");
        }
    }
    UpdateLanShareUiState();
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

    PersistWorkspaceUniformState(activeWorkspaceName_);
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

void Engine::HandlePipelineMoveCommand(const std::string& workspaceName, const int delta) {
    if (workspaceName.empty() || delta == 0) {
        return;
    }

    auto it = std::ranges::find(workspaceOrder_, workspaceName);
    if (it == workspaceOrder_.end()) {
        return;
    }

    const std::ptrdiff_t currentIndex = std::distance(workspaceOrder_.begin(), it);
    const std::ptrdiff_t targetIndex = std::clamp(currentIndex + static_cast<std::ptrdiff_t>(delta),
                                                  static_cast<std::ptrdiff_t>(0),
                                                  static_cast<std::ptrdiff_t>(workspaceOrder_.size() - 1));
    if (targetIndex == currentIndex) {
        return;
    }

    std::iter_swap(workspaceOrder_.begin() + currentIndex, workspaceOrder_.begin() + targetIndex);
    SyncWorkspaceUiState();
    UpdatePipelineUiState();
}

void Engine::HandlePipelineAddPassCommand(const std::string& workspaceName) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    workspaceIt->second.pipelineEnabled = true;
    std::erase(workspaceOrder_, workspaceName);
    workspaceOrder_.push_back(workspaceName);
    SyncWorkspaceUiState();
    UpdatePipelineUiState();
}

void Engine::HandlePipelineConfigChanged(const EditorUI::PipelineEditCommand& command) {
    auto workspaceIt = workspaces_.find(command.workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    auto& workspace = workspaceIt->second;
    switch (command.action) {
        case EditorUI::PipelineEditAction::SetOutputName: {
            std::string outputName = trim(command.outputName);
            if (outputName.empty()) {
                outputName = workspace.name;
            }
            for (char& c : outputName) {
                const bool valid = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
                if (!valid) {
                    c = '_';
                }
            }
            workspace.outputTextureName = outputName;
            break;
        }

        case EditorUI::PipelineEditAction::SetEnabled:
            workspace.pipelineEnabled = command.enabled;
            break;
    }
    UpdatePipelineUiState();
}

bool Engine::HandlePipelineSaveChainRequest(const std::string& path) const {
    if (path.empty()) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    const auto uniformTypeString = [](EditorUI::PipelineGlobalUniformType type) -> const char* {
        switch (type) {
            case EditorUI::PipelineGlobalUniformType::Float: return "float";
            case EditorUI::PipelineGlobalUniformType::Int: return "int";
            case EditorUI::PipelineGlobalUniformType::Bool: return "bool";
            case EditorUI::PipelineGlobalUniformType::Vec4: return "vec4";
        }
        return "float";
    };

    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"order\": [";
    bool wroteFirst = false;
    for (const auto& workspaceName : workspaceOrder_) {
        if (!workspaces_.contains(workspaceName)) {
            continue;
        }
        if (wroteFirst) {
            out << ", ";
        }
        wroteFirst = true;
        out << "\"" << EscapeJsonString(workspaceName) << "\"";
    }
    out << "],\n";

    out << "  \"passes\": [\n";
    bool wrotePass = false;
    for (const auto& workspaceName : workspaceOrder_) {
        const auto workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            continue;
        }
        if (wrotePass) {
            out << ",\n";
        }
        wrotePass = true;
        const auto& workspace = workspaceIt->second;
        out << "    {\"workspace\": \"" << EscapeJsonString(workspaceName) << "\", "
            << "\"enabled\": " << (workspace.pipelineEnabled ? "true" : "false") << ", "
            << "\"output\": \"" << EscapeJsonString(workspace.outputTextureName) << "\"}";
    }
    out << "\n  ],\n";

    out << "  \"connections\": [\n";
    bool wroteConnection = false;
    for (const auto& [targetWorkspace, perInput] : pipelineConnections_) {
        for (const auto& [targetInput, binding] : perInput) {
            if (wroteConnection) {
                out << ",\n";
            }
            wroteConnection = true;
            out << "    {\"target_workspace\": \"" << EscapeJsonString(targetWorkspace) << "\", "
                << "\"target_input\": \"" << EscapeJsonString(targetInput) << "\", "
                << "\"source_workspace\": \"" << EscapeJsonString(binding.sourceWorkspace) << "\", "
                << "\"source_slot\": \"" << EscapeJsonString(binding.sourceSlot) << "\"}";
        }
    }
    out << "\n  ],\n";

    out << "  \"global_uniforms\": [\n";
    bool wroteUniform = false;
    for (const auto& uniform : pipelineGlobalUniforms_) {
        if (wroteUniform) {
            out << ",\n";
        }
        wroteUniform = true;
        out << "    {\"name\": \"" << EscapeJsonString(uniform.name) << "\", "
            << "\"type\": \"" << uniformTypeString(uniform.type) << "\", "
            << "\"float\": " << uniform.floatValue << ", "
            << "\"int\": " << uniform.intValue << ", "
            << "\"bool\": " << (uniform.boolValue ? "true" : "false") << ", "
            << "\"vec4\": [" << uniform.vec4Value[0] << ", "
            << uniform.vec4Value[1] << ", "
            << uniform.vec4Value[2] << ", "
            << uniform.vec4Value[3] << "]}";
    }
    out << "\n  ]\n";
    out << "}\n";
    return out.good();
}

void Engine::HandlePipelineResetRequest() {
    workspaceOrder_.clear();
    workspaceOrder_.reserve(workspaces_.size());
    for (const auto& [name, workspace] : workspaces_) {
        (void)workspace;
        workspaceOrder_.push_back(name);
    }
    std::ranges::sort(workspaceOrder_);

    for (auto& [name, workspace] : workspaces_) {
        workspace.pipelineEnabled = true;
        workspace.outputTextureName = name;
    }
    pipelineConnections_.clear();
    pipelineGlobalUniforms_.clear();
    dependencyFlashUntil_.clear();

    SyncWorkspaceUiState();
    UpdatePipelineUiState();
}

void Engine::HandlePipelineOpenFileRequest(const std::string& workspaceName, const bool openCppFile) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    if (openCppFile) {
        SwitchToWorkspace(workspaceName, true);
        return;
    }

    SwitchToWorkspace(workspaceName, false);
    activeFilePath_ = workspaceIt->second.shaderPath;
    ui_->SetActiveDocument(activeFilePath_);
}

void Engine::HandlePipelineConnectionCommand(const EditorUI::PipelineConnectionCommand& command) {
    if (command.targetWorkspace.empty() || command.targetSlot.empty()) {
        return;
    }
    if (!workspaces_.contains(command.targetWorkspace)) {
        return;
    }

    if (command.clear) {
        if (auto workspaceIt = pipelineConnections_.find(command.targetWorkspace);
            workspaceIt != pipelineConnections_.end()) {
            workspaceIt->second.erase(command.targetSlot);
            if (workspaceIt->second.empty()) {
                pipelineConnections_.erase(workspaceIt);
            }
        }
        UpdatePipelineUiState();
        return;
    }

    if (command.sourceSlot.empty()) {
        return;
    }

    std::string resolvedSourceWorkspace = command.sourceWorkspace;
    if (resolvedSourceWorkspace.empty()) {
        for (const auto& workspaceName : workspaceOrder_) {
            const auto workspaceIt = workspaces_.find(workspaceName);
            if (workspaceIt == workspaces_.end()) {
                continue;
            }
            const std::string outputName = workspaceIt->second.outputTextureName.empty()
                                               ? workspaceName
                                               : workspaceIt->second.outputTextureName;
            if (outputName == command.sourceSlot || workspaceName == command.sourceSlot) {
                resolvedSourceWorkspace = workspaceName;
                break;
            }
        }
    }

    pipelineConnections_[command.targetWorkspace][command.targetSlot] = PipelineConnectionBinding{
        resolvedSourceWorkspace,
        command.sourceSlot
    };
    UpdatePipelineUiState();
}

void Engine::HandlePipelineGlobalUniformCommand(const EditorUI::PipelineGlobalUniformCommand& command) {
    if (command.uniform.name.empty()) {
        return;
    }

    if (command.action == EditorUI::PipelineGlobalUniformCommand::Action::Remove) {
        std::erase_if(pipelineGlobalUniforms_, [&](const EditorUI::PipelineGlobalUniformView& uniform) {
            return uniform.name == command.uniform.name;
        });
        UpdatePipelineUiState();
        return;
    }

    auto existing = std::ranges::find_if(pipelineGlobalUniforms_,
                                         [&](const EditorUI::PipelineGlobalUniformView& uniform) {
                                             return uniform.name == command.uniform.name;
                                         });
    if (existing == pipelineGlobalUniforms_.end()) {
        pipelineGlobalUniforms_.push_back(command.uniform);
    } else {
        *existing = command.uniform;
    }
    UpdatePipelineUiState();
}

bool Engine::ExportPipelineResource(const std::string& resourceName, const std::string& targetPath) const {
    if (resourceName.empty() || targetPath.empty()) {
        return false;
    }
    const auto resourceIt = sharedTextures_.find(resourceName);
    if (resourceIt == sharedTextures_.end()) {
        return false;
    }
    if (resourceIt->second.texture == 0 || resourceIt->second.width <= 0 || resourceIt->second.height <= 0) {
        return false;
    }

    const int width = resourceIt->second.width;
    const int height = resourceIt->second.height;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);

    GLint previousPackAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, resourceIt->second.texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);

    std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y) {
        const std::size_t rowOffset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        for (int x = 0; x < width; ++x) {
            const std::size_t pixelOffset = rowOffset + static_cast<std::size_t>(x) * 4u;
            out.put(static_cast<char>(rgba[pixelOffset + 0]));
            out.put(static_cast<char>(rgba[pixelOffset + 1]));
            out.put(static_cast<char>(rgba[pixelOffset + 2]));
        }
    }
    return out.good();
}

void Engine::HandleCompileFailure(const CompileResult& result) {
    DebugLog("HandleCompileFailure(workspace=" + result.workspaceName +
             ", sourceHash=" + std::to_string(result.sourceHash) + ")");
    ui_->AddLogOutput("[JIT] Compile failed for workspace '" + result.workspaceName +
                      "'. Keeping previous program.");
    if (const auto hashIt = latestSourceHashes_.find(result.workspaceName);
        hashIt != latestSourceHashes_.end() && hashIt->second == result.sourceHash) {
        compileFailures_[result.workspaceName] = CompileFailureState{ result.sourceHash, false };
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
    DebugLog("HandleCompileSuccess(workspace=" + result.workspaceName +
             ", sourceHash=" + std::to_string(result.sourceHash) +
             ", stateAbiHash=" + std::to_string(result.stateAbiHash) +
             ", program_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(result.program.get())) +
             ", uniforms=" + std::to_string(result.uniformDescriptors.size()) + ")");
    ui_->SetCompilationStatus(false, ActiveWorkspaceHasCompileError(), false);
    recentErrors_.erase(result.workspaceName);

    auto workspaceIt = workspaces_.find(result.workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    if (const auto hashIt = latestSourceHashes_.find(result.workspaceName);
        hashIt != latestSourceHashes_.end()) {
        const std::size_t latestHash = hashIt->second;
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

    const std::uint64_t previousAbiHash = workspaceIt->second.stateAbiHash;
    const bool abiHashChanged = (previousAbiHash != result.stateAbiHash);
    workspaceIt->second.stateAbiHash = result.stateAbiHash;
    workspaceIt->second.arenaOffset = 0;
    if (abiHashChanged) {
        ResetWorkspaceRuntimeState(&workspaceIt->second, false);
        workspaceIt->second.stateAbiHash = result.stateAbiHash;
        if (previousAbiHash != 0) {
            ui_->AddLogOutput("[Runtime] State ABI changed for workspace '" + result.workspaceName +
                              "'. Runtime state was reset.");
        }
    }

    workspaceIt->second.uniforms.Rebuild(result.uniformDescriptors);
    workspaceIt->second.samplerUniformNames = result.samplerUniformNames;
    workspaceIt->second.storageBufferNames = result.storageBufferNames;
    workspaceIt->second.shaderDependencies = result.shaderDependencies;
    workspaceIt->second.samplerLocationProgram = 0;
    workspaceIt->second.samplerLocationCache.clear();
    workspaceIt->second.globalUniformLocationProgram = 0;
    workspaceIt->second.globalUniformLocationCache.clear();
    if (workspaceIt->second.outputTextureName.empty()) {
        workspaceIt->second.outputTextureName = workspaceIt->second.name;
    }
    if (result.workspaceName == activeWorkspaceName_) {
        ctx_.reset_arena();
        ui_->SetUniformValues(workspaceIt->second.uniforms.Values());
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
    DebugLog("HandleCompileSuccess activated(workspace=" + result.workspaceName +
             ", ctx.state_i0=" + std::to_string(ctx_.state_i[0]) +
             ", glIsProgram=" + std::to_string(ctx_.state_i[0] != 0 &&
                                               glIsProgram(static_cast<GLuint>(ctx_.state_i[0])) == GL_TRUE) + ")");
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

bool Engine::ResolvePipelineSamplerBinding(const WorkspaceState& workspace,
                                           const std::string& samplerName,
                                           std::string* outResourceName,
                                           bool* outIsExplicit) const {
    if (outResourceName == nullptr || outIsExplicit == nullptr) {
        return false;
    }
    outResourceName->clear();
    *outIsExplicit = false;

    if (auto currentIt = std::ranges::find(workspaceOrder_, workspace.name);
        currentIt != workspaceOrder_.end()) {
        for (auto it = currentIt; it != workspaceOrder_.begin();) {
            --it;

            const auto predecessorIt = workspaces_.find(*it);
            if (predecessorIt == workspaces_.end() || !predecessorIt->second.pipelineEnabled) {
                continue;
            }

            const std::string outputName = predecessorIt->second.outputTextureName.empty()
                                               ? predecessorIt->second.name
                                               : predecessorIt->second.outputTextureName;
            if (sharedTextures_.contains(outputName)) {
                *outResourceName = outputName;
                *outIsExplicit = false;
                return true;
            }
            if (sharedTextures_.contains(predecessorIt->second.name)) {
                *outResourceName = predecessorIt->second.name;
                *outIsExplicit = false;
                return true;
            }
        }
    }

    if (sharedTextures_.contains(samplerName)) {
        *outResourceName = samplerName;
        *outIsExplicit = false;
        return true;
    }

    return false;
}

void Engine::BindSharedSamplerUniforms(WorkspaceState* workspace, const GLuint programHandle) {
    if (workspace == nullptr || programHandle == 0 || workspace->samplerUniformNames.empty()) {
        return;
    }

    // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS is a fixed device property — cache the first valid
    // value to avoid a driver round-trip on every pass.
    static GLint maxUnits = 0;
    if (maxUnits == 0) {
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
    }
    if (maxUnits <= 0) {
        return;
    }

    int textureUnit = 0;
    if (workspace->samplerLocationProgram != programHandle) {
        workspace->samplerLocationProgram = programHandle;
        workspace->samplerLocationCache.clear();
    }

    for (const auto& samplerName : workspace->samplerUniformNames) {
        std::string resourceName;
        bool explicitBinding = false;
        GLint location = -1;
        if (const auto cacheIt = workspace->samplerLocationCache.find(samplerName);
            cacheIt != workspace->samplerLocationCache.end()) {
            location = cacheIt->second;
        } else {
            location = glGetUniformLocation(programHandle, samplerName.c_str());
            workspace->samplerLocationCache[samplerName] = location;
        }
        if (location < 0) {
            continue;
        }

        GLuint textureToBind = 0;
        if (ResolvePipelineSamplerBinding(*workspace, samplerName, &resourceName, &explicitBinding)) {
            (void)explicitBinding;
            if (const auto resourceIt = sharedTextures_.find(resourceName);
                resourceIt != sharedTextures_.end()) {
                textureToBind = resourceIt->second.texture;
            }
        }

        const int assignedUnit = (textureUnit < maxUnits) ? textureUnit : (maxUnits - 1);
        glActiveTexture(GL_TEXTURE0 + assignedUnit);
        glBindTexture(GL_TEXTURE_2D, textureToBind);
        glUniform1i(location, assignedUnit);
        if (textureUnit < maxUnits) {
            ++textureUnit;
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

void Engine::ApplyPipelineGlobalUniforms(WorkspaceState* workspace, const GLuint programHandle) {
    if (workspace == nullptr || programHandle == 0 || pipelineGlobalUniforms_.empty()) {
        return;
    }

    if (workspace->globalUniformLocationProgram != programHandle) {
        workspace->globalUniformLocationProgram = programHandle;
        workspace->globalUniformLocationCache.clear();
    }

    for (const auto& uniform : pipelineGlobalUniforms_) {
        if (uniform.name.empty()) {
            continue;
        }

        GLint location = -1;
        if (const auto locationIt = workspace->globalUniformLocationCache.find(uniform.name);
            locationIt != workspace->globalUniformLocationCache.end()) {
            location = locationIt->second;
        } else {
            location = glGetUniformLocation(programHandle, uniform.name.c_str());
            workspace->globalUniformLocationCache[uniform.name] = location;
        }
        if (location < 0) {
            continue;
        }

        switch (uniform.type) {
            case EditorUI::PipelineGlobalUniformType::Float:
                glUniform1f(location, uniform.floatValue);
                break;
            case EditorUI::PipelineGlobalUniformType::Int:
                glUniform1i(location, uniform.intValue);
                break;
            case EditorUI::PipelineGlobalUniformType::Bool:
                glUniform1i(location, uniform.boolValue ? 1 : 0);
                break;
            case EditorUI::PipelineGlobalUniformType::Vec4:
                glUniform4f(location,
                            uniform.vec4Value[0],
                            uniform.vec4Value[1],
                            uniform.vec4Value[2],
                            uniform.vec4Value[3]);
                break;
        }
    }
}

void Engine::ApplyWorkspaceUniforms(const std::string& workspaceName) {
    auto workspaceIt = workspaces_.find(workspaceName);
    if (workspaceIt == workspaces_.end()) {
        return;
    }

    const GLuint programHandle = static_cast<GLuint>(ctx_.state_i[0]);
    if (programHandle == 0) {
        workspaceIt->second.uniforms.BindProgram(0);
        return;
    }

    workspaceIt->second.uniforms.BindProgram(programHandle);
    glUseProgram(programHandle);
    BindSharedSamplerUniforms(&workspaceIt->second, programHandle);
    workspaceIt->second.uniforms.UploadDirty();
    ApplyPipelineGlobalUniforms(&workspaceIt->second, programHandle);
}

void Engine::UpdatePipelineUiState() {
    if (!ui_) {
        return;
    }

    std::vector<EditorUI::PipelinePassView> passViews;
    passViews.reserve(workspaceOrder_.size());
    std::unordered_map<std::string, std::vector<std::string>> outputOwners;
    outputOwners.reserve(workspaceOrder_.size() * 2);
    for (const auto& workspaceName : workspaceOrder_) {
        const auto workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            continue;
        }

        EditorUI::PipelinePassView view;
        view.workspaceName = workspaceName;
        view.outputName = workspaceIt->second.outputTextureName.empty() ? workspaceName
                                                                         : workspaceIt->second.outputTextureName;
        view.cppPath = workspaceIt->second.cppPath;
        view.shaderPath = workspaceIt->second.shaderPath;
        view.inputSamplers = workspaceIt->second.samplerUniformNames;
        view.inputBuffers = workspaceIt->second.storageBufferNames;
        view.outputTextures.push_back(view.outputName);
        if (view.outputName != workspaceName) {
            view.outputTextures.push_back(workspaceName);
        }
        view.enabled = workspaceIt->second.pipelineEnabled;
        view.active = (workspaceName == activeWorkspaceName_);
        view.compiled = compiledPrograms_.contains(workspaceName);
        view.hasCompileError = compileFailures_.contains(workspaceName);
        view.gpuTimeMs = workspaceIt->second.lastGpuPassTimeMs;
        ShaderSections shaderSections;
        const bool parsedShader = parseShaderSections(workspaceIt->second.shaderSource, &shaderSections);
        const bool hasCompute = parsedShader && !shaderSections.compute.empty();
        const bool hasRaster = parsedShader && (!shaderSections.vertex.empty() || !shaderSections.fragment.empty());
        view.passType = hasCompute && hasRaster ? EditorUI::PipelinePassType::Hybrid
                                                : hasCompute ? EditorUI::PipelinePassType::Compute
                                                             : EditorUI::PipelinePassType::Raster;
        passViews.push_back(std::move(view));
        outputOwners[passViews.back().outputName].push_back(passViews.back().workspaceName);
        outputOwners[passViews.back().workspaceName].push_back(passViews.back().workspaceName);
    }

    std::vector<EditorUI::PipelineResourceView> resources;
    resources.reserve(sharedTextures_.size());
    for (const auto& [name, resource] : sharedTextures_) {
        EditorUI::PipelineResourceView view;
        view.name = name;
        view.texture = resource.texture;
        view.width = resource.width;
        view.height = resource.height;
        resources.push_back(std::move(view));
    }
    std::ranges::sort(resources, [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });

    std::vector<EditorUI::PipelineConnectionView> connections;
    std::unordered_set<std::string> explicitTargetInputs;
    explicitTargetInputs.reserve(64);
    for (const auto& [targetWorkspace, perInput] : pipelineConnections_) {
        for (const auto& [targetInput, binding] : perInput) {
            if (binding.sourceSlot.empty()) {
                continue;
            }
            EditorUI::PipelineConnectionView connection;
            connection.sourceWorkspace = binding.sourceWorkspace.empty() ? "<resource>" : binding.sourceWorkspace;
            connection.sourceSlot = binding.sourceSlot;
            connection.targetWorkspace = targetWorkspace;
            connection.targetSlot = targetInput;
            connection.explicitConnection = true;
            connections.push_back(std::move(connection));
            explicitTargetInputs.insert(PipelineSlotKey(targetWorkspace, targetInput));
        }
    }

    for (const auto& pass : passViews) {
        for (const auto& inputName : pass.inputSamplers) {
            const std::string key = PipelineSlotKey(pass.workspaceName, inputName);
            if (explicitTargetInputs.contains(key)) {
                continue;
            }
            auto ownerIt = outputOwners.find(inputName);
            if (ownerIt == outputOwners.end() || ownerIt->second.empty()) {
                continue;
            }
            EditorUI::PipelineConnectionView connection;
            connection.sourceWorkspace = ownerIt->second.front();
            connection.sourceSlot = inputName;
            connection.targetWorkspace = pass.workspaceName;
            connection.targetSlot = inputName;
            connection.explicitConnection = false;
            connections.push_back(std::move(connection));
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> dependenciesByPath;
    dependenciesByPath.reserve(workspaces_.size() * 2);
    for (const auto& [workspaceName, workspace] : workspaces_) {
        dependenciesByPath[NormalizePathString(workspace.shaderPath)].push_back(workspaceName);
        for (const auto& dependencyPath : workspace.shaderDependencies) {
            dependenciesByPath[dependencyPath].push_back(workspaceName);
        }
    }

    std::vector<EditorUI::PipelineDependencyView> dependencyViews;
    dependencyViews.reserve(dependenciesByPath.size());
    const double now = glfwGetTime();
    for (auto& [path, dependents] : dependenciesByPath) {
        std::ranges::sort(dependents);
        dependents.erase(std::unique(dependents.begin(), dependents.end()), dependents.end());

        EditorUI::PipelineDependencyView view;
        view.path = path;
        view.dependentWorkspaces = std::move(dependents);
        if (auto flashIt = dependencyFlashUntil_.find(path); flashIt != dependencyFlashUntil_.end()) {
            view.flashing = now < flashIt->second;
        }
        dependencyViews.push_back(std::move(view));
    }
    std::ranges::sort(dependencyViews, [](const auto& lhs, const auto& rhs) {
        return lhs.path < rhs.path;
    });

    ui_->SetPipelinePasses(std::move(passViews));
    ui_->SetPipelineResources(std::move(resources));
    ui_->SetPipelineConnections(std::move(connections));
    ui_->SetPipelineDependencies(std::move(dependencyViews));
    ui_->SetPipelineGlobalUniforms(pipelineGlobalUniforms_);
}

void Engine::RenderSceneToTexture() {
    if (sceneWidth_ <= 0 || sceneHeight_ <= 0) {
        sharedTextures_.clear();
        UpdatePipelineUiState();
        return;
    }

    const bool renderActiveWorkspaceOnly =
        (ui_ != nullptr && ui_->GetRendererOutputMode() == EditorUI::RendererOutputMode::ActiveWorkspace);
    if (renderActiveWorkspaceOnly) {
        sharedTextures_.clear();
        if (sceneFbo_ == 0 || sceneColorTex_ == 0) {
            UpdatePipelineUiState();
            return;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
        glViewport(0, 0, sceneWidth_, sceneHeight_);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (!activeWorkspaceName_.empty()) {
            auto workspaceIt = workspaces_.find(activeWorkspaceName_);
            auto programIt = compiledPrograms_.find(activeWorkspaceName_);
            if (workspaceIt != workspaces_.end() &&
                programIt != compiledPrograms_.end() &&
                programIt->second) {
                auto& workspace = workspaceIt->second;
                const auto& program = programIt->second;

                LoadWorkspaceRuntimeState(activeWorkspaceName_);
                ActivateWorkspaceGeometry(activeWorkspaceName_);
                ctx_.width = sceneWidth_;
                ctx_.height = sceneHeight_;
                InitializeProgramIfNeeded(program);

                const int readQueryIndex = 1 - workspace.gpuQueryWriteIndex;
                if (workspace.gpuQueryPending[static_cast<std::size_t>(readQueryIndex)]) {
                    GLuint available = GL_FALSE;
                    glGetQueryObjectuiv(workspace.gpuQueries[static_cast<std::size_t>(readQueryIndex)],
                                        GL_QUERY_RESULT_AVAILABLE,
                                        &available);
                    if (available == GL_TRUE) {
                        GLuint64 elapsedNanoseconds = 0;
                        glGetQueryObjectui64v(workspace.gpuQueries[static_cast<std::size_t>(readQueryIndex)],
                                              GL_QUERY_RESULT,
                                              &elapsedNanoseconds);
                        workspace.lastGpuPassTimeMs =
                            static_cast<float>(static_cast<double>(elapsedNanoseconds) / 1000000.0);
                        workspace.gpuQueryPending[static_cast<std::size_t>(readQueryIndex)] = false;
                    }
                }

                const int queryIndex = workspace.gpuQueryWriteIndex;
                glBeginQuery(GL_TIME_ELAPSED, workspace.gpuQueries[static_cast<std::size_t>(queryIndex)]);

                bool dispatchedCompute = false;
                if (program->functions.compute) {
                    program->functions.compute(&ctx_);
                    dispatchedCompute = true;
                }
                if (dispatchedCompute) {
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
                }

                if (program->functions.update) {
                    program->functions.update(&ctx_);
                }
                ApplyWorkspaceUniforms(activeWorkspaceName_);
                if (program->functions.render) {
                    program->functions.render(&ctx_);
                }

                glEndQuery(GL_TIME_ELAPSED);
                workspace.gpuQueryPending[static_cast<std::size_t>(queryIndex)] = true;
                workspace.gpuQueryWriteIndex = 1 - workspace.gpuQueryWriteIndex;
                SaveWorkspaceRuntimeState(activeWorkspaceName_);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ui_->SetRendererTexture(sceneColorTex_, sceneWidth_, sceneHeight_);
        UpdatePipelineUiState();

        if (!activeWorkspaceName_.empty()) {
            LoadWorkspaceRuntimeState(activeWorkspaceName_);
            ActivateWorkspaceGeometry(activeWorkspaceName_);
            ctx_.width = sceneWidth_;
            ctx_.height = sceneHeight_;
        }
        return;
    }

    // Publish previous-frame resources before the frame starts (for temporal sampling).
    sharedTextures_.clear();
    for (auto& [workspaceName, workspace] : workspaces_) {
        const std::string outputName = workspace.outputTextureName.empty() ? workspaceName : workspace.outputTextureName;
        const int prevFrameIndex = 1 - workspace.passWriteIndex;
        const GLuint prevFrameTexture = workspace.passColorTextures[static_cast<std::size_t>(prevFrameIndex)];
        if (prevFrameTexture == 0) {
            continue;
        }

        const SharedTextureResource resource{ prevFrameTexture, sceneWidth_, sceneHeight_ };
        sharedTextures_[outputName] = resource;
        sharedTextures_[workspaceName] = resource;
        sharedTextures_[outputName + "_prev"] = resource;
        sharedTextures_[workspaceName + "_prev"] = resource;
    }

    GLuint finalTexture = 0;
    int finalWidth = sceneWidth_;
    int finalHeight = sceneHeight_;

    for (const auto& workspaceName : workspaceOrder_) {
        auto workspaceIt = workspaces_.find(workspaceName);
        if (workspaceIt == workspaces_.end()) {
            continue;
        }
        auto& workspace = workspaceIt->second;
        if (!workspace.pipelineEnabled) {
            continue;
        }

        auto programIt = compiledPrograms_.find(workspaceName);
        if (programIt == compiledPrograms_.end() || !programIt->second) {
            continue;
        }

        if (!EnsureWorkspacePassTargets(&workspace, sceneWidth_, sceneHeight_)) {
            continue;
        }

        LoadWorkspaceRuntimeState(workspaceName);
        ActivateWorkspaceGeometry(workspaceName);
        ctx_.width = sceneWidth_;
        ctx_.height = sceneHeight_;

        const auto& program = programIt->second;
        InitializeProgramIfNeeded(program);

        const int readQueryIndex = 1 - workspace.gpuQueryWriteIndex;
        if (workspace.gpuQueryPending[static_cast<std::size_t>(readQueryIndex)]) {
            GLuint available = GL_FALSE;
            glGetQueryObjectuiv(workspace.gpuQueries[static_cast<std::size_t>(readQueryIndex)],
                                GL_QUERY_RESULT_AVAILABLE,
                                &available);
            if (available == GL_TRUE) {
                GLuint64 elapsedNanoseconds = 0;
                glGetQueryObjectui64v(workspace.gpuQueries[static_cast<std::size_t>(readQueryIndex)],
                                      GL_QUERY_RESULT,
                                      &elapsedNanoseconds);
                workspace.lastGpuPassTimeMs = static_cast<float>(static_cast<double>(elapsedNanoseconds) / 1000000.0);
                workspace.gpuQueryPending[static_cast<std::size_t>(readQueryIndex)] = false;
            }
        }

        const int writeIndex = workspace.passWriteIndex;
        const GLuint passFbo = workspace.passFbos[static_cast<std::size_t>(writeIndex)];
        glBindFramebuffer(GL_FRAMEBUFFER, passFbo);
        glViewport(0, 0, sceneWidth_, sceneHeight_);

        const int queryIndex = workspace.gpuQueryWriteIndex;
        glBeginQuery(GL_TIME_ELAPSED, workspace.gpuQueries[static_cast<std::size_t>(queryIndex)]);

        bool dispatchedCompute = false;
        if (program->functions.compute) {
            program->functions.compute(&ctx_);
            dispatchedCompute = true;
        }
        if (dispatchedCompute) {
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        }

        if (program->functions.update) {
            program->functions.update(&ctx_);
        }
        ApplyWorkspaceUniforms(workspaceName);
        if (program->functions.render) {
            program->functions.render(&ctx_);
        }

        glEndQuery(GL_TIME_ELAPSED);
        workspace.gpuQueryPending[static_cast<std::size_t>(queryIndex)] = true;
        workspace.gpuQueryWriteIndex = 1 - workspace.gpuQueryWriteIndex;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        finalTexture = workspace.passColorTextures[static_cast<std::size_t>(writeIndex)];
        finalWidth = sceneWidth_;
        finalHeight = sceneHeight_;

        const std::string outputName = workspace.outputTextureName.empty() ? workspaceName : workspace.outputTextureName;
        const SharedTextureResource outputResource{ finalTexture, sceneWidth_, sceneHeight_ };
        sharedTextures_[outputName] = outputResource;
        sharedTextures_[workspaceName] = outputResource;

        const int prevFrameIndex = 1 - writeIndex;
        const GLuint prevFrameTexture = workspace.passColorTextures[static_cast<std::size_t>(prevFrameIndex)];
        if (prevFrameTexture != 0) {
            const SharedTextureResource prevResource{ prevFrameTexture, sceneWidth_, sceneHeight_ };
            sharedTextures_[outputName + "_prev"] = prevResource;
            sharedTextures_[workspaceName + "_prev"] = prevResource;
        }

        workspace.passWriteIndex = prevFrameIndex;
        SaveWorkspaceRuntimeState(workspaceName);
    }

    if (finalTexture == 0) {
        if (sceneFbo_ == 0) {
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
        glViewport(0, 0, sceneWidth_, sceneHeight_);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        finalTexture = sceneColorTex_;
        finalWidth = sceneWidth_;
        finalHeight = sceneHeight_;
    }

    ui_->SetRendererTexture(finalTexture, finalWidth, finalHeight);
    UpdatePipelineUiState();

    if (!activeWorkspaceName_.empty()) {
        LoadWorkspaceRuntimeState(activeWorkspaceName_);
        ActivateWorkspaceGeometry(activeWorkspaceName_);
        ctx_.width = sceneWidth_;
        ctx_.height = sceneHeight_;
    }
}

void Engine::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        if (ctx_.consume_reset_state_request()) {
            HardResetActiveWorkspaceState("requested by scene code", false);
        }

        const double now = glfwGetTime();
        ProcessPendingReloads(now);
        CompletePendingJITReset();
        SubmitDueCompiles(now);
        ProcessCompileResults();

        const double frameDeltaSeconds = std::max(0.0, now - lastTime_);
        ctx_.deltaTime = static_cast<float>(frameDeltaSeconds);
        ctx_.time = 0.0f;
        auto activeWorkspaceIt = workspaces_.find(activeWorkspaceName_);
        if (activeWorkspaceIt != workspaces_.end()) {
            auto& activeWorkspace = activeWorkspaceIt->second;
            AdvanceWorkspacePlayback(&activeWorkspace, now);
            if (activeWorkspace.playbackPaused) {
                ctx_.deltaTime = 0.0f;
            } else {
                ctx_.deltaTime = static_cast<float>(frameDeltaSeconds * static_cast<double>(activeWorkspace.playbackSpeed));
            }
            ctx_.time = static_cast<float>(activeWorkspace.accumulatedActiveSeconds);
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

        // Iterator is still valid here: workspaces_ is only mutated by Register/Delete/Shutdown,
        // none of which run in the render-side path between the find above and this use.
        if (activeWorkspaceIt != workspaces_.end()) {
            ui_->SetUniformValues(activeWorkspaceIt->second.uniforms.Values());
            ui_->SetPlaybackState(BuildPlaybackStateForWorkspace(activeWorkspaceName_));
        } else {
            ui_->SetUniformValues({});
            ui_->SetPlaybackState(EditorUI::PlaybackState{});
        }

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

    for (auto& [workspaceName, workspace] : workspaces_) {
        PersistWorkspaceUniformState(workspaceName);
        ReleaseWorkspaceGeometry(&workspace);
        ReleaseWorkspacePassTargets(&workspace);
    }

    compileFailures_.clear();
    compileRetryAfter_.clear();
    recentErrors_.clear();
    ignoreWatcherUntil_.clear();
    latestSources_.clear();
    latestSourceHashes_.clear();
    latestUniformDescriptors_.clear();
    latestSamplerUniformNames_.clear();
    latestStorageBufferNames_.clear();
    latestShaderDependencies_.clear();
    latestStateAbiHashes_.clear();
    pendingCompilesAt_.clear();
    inFlightSourceHashes_.clear();
    workspaces_.clear();
    workspaceOrder_.clear();
    fileToWorkspace_.clear();
    workspaceDirty_.clear();
    activeWorkspaceName_.clear();
    activeFilePath_.clear();
    pipelineConnections_.clear();
    pipelineGlobalUniforms_.clear();
    dependencyFlashUntil_.clear();

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

    if (ctx_.vao != 0 && glIsVertexArray(ctx_.vao) == GL_TRUE) {
        glDeleteVertexArrays(1, &ctx_.vao);
    }
    ctx_.vao = 0;

    if (ctx_.vbo != 0 && glIsBuffer(ctx_.vbo) == GL_TRUE) {
        glDeleteBuffers(1, &ctx_.vbo);
    }
    ctx_.vbo = 0;

    if (ctx_.defaultShader) {
        glDeleteProgram(ctx_.defaultShader);
        ctx_.defaultShader = 0;
    }

    // Runtime pointers/state are host-owned; clear them on shutdown.
    ctx_.clear_runtime_state();
    ctx_.arena_base = nullptr;
    ctx_.arena_size = 0;
    ctx_.reset_state_requested = false;

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
