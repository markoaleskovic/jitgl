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
#include <filesystem>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>

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

    constexpr double COMPILE_DEBOUNCE_SECONDS = 0.15;
    constexpr double WATCHER_IGNORE_SECONDS = 0.75;
    constexpr double COMPILE_FAILURE_RETRY_SECONDS = 1.0;
}

Engine::Engine() = default;
Engine::~Engine() { Shutdown(); }

bool Engine::Init() {
    if (!InitWindow()) return false;
    if (!InitGL()) return false;
    if (!InitUI()) return false;
    if (!InitJIT()) return false;
    if (!InitWatcher()) return false;

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

    const float quadVerts[] = {
        -1.f, -1.f, 1.f, -1.f, 1.f, 1.f,
        -1.f, -1.f, 1.f, 1.f, -1.f, 1.f,
    };
    glGenVertexArrays(1, &ctx_.vao);
    glGenBuffers(1, &ctx_.vbo);
    glBindVertexArray(ctx_.vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

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

    return true;
}

bool Engine::InitJIT() {
    jit_ = std::make_unique<JitEngine>();
    jit_->SetOutputCallback([this](const std::string& msg) {
        ui_->AddLogOutput(msg);
    });

    if (!jit_->Init(PREAMBLE_PATH)) {
        ui_->AddLogOutput("[Engine] JIT engine failed to initialize.");
        return false;
    }

    compileThreadRunning_.store(true);
    compileThread_ = std::thread([this]() {
        CompileThreadMain();
    });

    return true;
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

    auto files = workspaceManager_->LoadAllFiles();
    for (const auto& file : files) {
        latestSources_[file.filepath] = file.content;
        ui_->AddDocument(file.filename, file.filepath, file.content);
    }

    if (!files.empty()) {
        activeFilePath_ = files.front().filepath;
        QueueCompile(activeFilePath_, files.front().content, glfwGetTime(), true);
    }

    watcher_ = std::make_unique<FileWatcher>(
        WORKSPACE_DIR,
        [this](const std::string& path) { OnFileChanged(path); }
    );
    watcher_->Start();

    return true;
}

void Engine::OnFileChanged(const std::string& filepath) {
    const auto extension = std::filesystem::path(filepath).extension().string();
    if (extension != ".cpp") {
        return;
    }

    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingReloads_.push(filepath);
}

void Engine::ProcessPendingReloads(double nowSeconds) {
    std::vector<std::string> filesToProcess;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
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

        auto latestIt = latestSources_.find(filepath);
        if (latestIt != latestSources_.end() && latestIt->second == *content) {
            continue;
        }

        latestSources_[filepath] = *content;
        ui_->AddDocument(std::filesystem::path(filepath).filename().string(), filepath, *content);
        QueueCompile(filepath, *content, nowSeconds, filepath == activeFilePath_);
        ui_->AddLogOutput("[Watcher] External change detected: " + filepath);
    }
}

void Engine::HandleDocumentEdited(const std::string& filepath, const std::string& content) {
    QueueCompile(filepath, content, glfwGetTime(), false);
}

void Engine::HandleActiveDocumentChanged(const std::string& filepath, const std::string& content) {
    activeFilePath_ = filepath;
    latestSources_[filepath] = content;

    if (!ActivateProgramForPath(filepath)) {
        QueueCompile(filepath, content, glfwGetTime(), true);
    }
}

void Engine::QueueCompile(const std::string& filepath, const std::string& source, double nowSeconds, bool immediate) {
    const std::size_t sourceHash = std::hash<std::string>{}(source);
    auto failureIt = compileFailures_.find(filepath);
    if (failureIt != compileFailures_.end() && failureIt->second.sourceHash == sourceHash) {
        if (!failureIt->second.suppressionLogged) {
            ui_->AddLogOutput("[JIT] Skipping unchanged invalid source for " + filepath + ". Waiting for edits.");
            failureIt->second.suppressionLogged = true;
        }
        pendingCompilesAt_.erase(filepath);
        return;
    }
    if (failureIt != compileFailures_.end() && failureIt->second.sourceHash != sourceHash) {
        compileFailures_.erase(failureIt);
    }

    latestSources_[filepath] = source;
    double requestedAt = immediate ? nowSeconds : (nowSeconds + COMPILE_DEBOUNCE_SECONDS);
    auto retryIt = compileRetryAfter_.find(filepath);
    if (retryIt != compileRetryAfter_.end() && requestedAt < retryIt->second) {
        requestedAt = retryIt->second;
    }
    pendingCompilesAt_[filepath] = requestedAt;
}

void Engine::SubmitDueCompiles(double nowSeconds) {
    std::vector<std::string> duePaths;
    for (const auto& pending : pendingCompilesAt_) {
        if (pending.second <= nowSeconds) {
            duePaths.push_back(pending.first);
        }
    }

    if (duePaths.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(compileMutex_);
        for (const auto& path : duePaths) {
            auto sourceIt = latestSources_.find(path);
            if (sourceIt == latestSources_.end()) {
                continue;
            }
            const std::size_t sourceHash = std::hash<std::string>{}(sourceIt->second);

            auto inFlightIt = inFlightSourceHashes_.find(path);
            if (inFlightIt != inFlightSourceHashes_.end() && inFlightIt->second == sourceHash) {
                pendingCompilesAt_.erase(path);
                continue;
            }

            const bool alreadyQueued = std::any_of(compileJobs_.begin(), compileJobs_.end(), [&](const CompileJob& job) {
                return job.filepath == path && job.sourceHash == sourceHash;
            });
            if (alreadyQueued) {
                pendingCompilesAt_.erase(path);
                continue;
            }

            compileJobs_.erase(std::remove_if(compileJobs_.begin(), compileJobs_.end(),
                                              [&](const CompileJob& job) {
                                                  return job.filepath == path;
                                              }),
                               compileJobs_.end());
            compileJobs_.push_back(CompileJob{ path, sourceIt->second, sourceHash });
            pendingCompilesAt_.erase(path);
        }
    }

    compileCv_.notify_one();
}

void Engine::CompileThreadMain() {
    while (compileThreadRunning_.load()) {
        CompileJob job;
        {
            std::unique_lock<std::mutex> lock(compileMutex_);
            compileCv_.wait(lock, [this]() {
                return !compileThreadRunning_.load() || !compileJobs_.empty();
            });

            if (!compileThreadRunning_.load()) {
                break;
            }

            job = std::move(compileJobs_.front());
            compileJobs_.pop_front();
            inFlightSourceHashes_[job.filepath] = job.sourceHash;
        }

        auto program = jit_->CompileSource(job.filepath, job.source);
        {
            std::lock_guard<std::mutex> lock(compileMutex_);
            inFlightSourceHashes_.erase(job.filepath);
            compileResults_.push(CompileResult{ job.filepath, std::move(program), job.sourceHash });
        }
    }
}

bool Engine::ActivateProgramForPath(const std::string& filepath) {
    auto it = compiledPrograms_.find(filepath);
    if (it == compiledPrograms_.end()) {
        return false;
    }

    activeProgram_ = it->second;
    if (activeProgram_ && activeProgram_->functions.init) {
        activeProgram_->functions.init(&ctx_);
    }

    ui_->AddLogOutput("[Runtime] Active scene switched to " + filepath);
    return true;
}

void Engine::ProcessCompileResults() {
    std::queue<CompileResult> localResults;
    {
        std::lock_guard<std::mutex> lock(compileMutex_);
        std::swap(localResults, compileResults_);
    }

    while (!localResults.empty()) {
        CompileResult result = std::move(localResults.front());
        localResults.pop();

        if (!result.program) {
            ui_->AddLogOutput("[JIT] Compile failed for " + result.filepath + ". Keeping previous program.");
            auto sourceIt = latestSources_.find(result.filepath);
            if (sourceIt != latestSources_.end()) {
                const std::size_t currentHash = std::hash<std::string>{}(sourceIt->second);
                if (currentHash == result.sourceHash) {
                    compileFailures_[result.filepath] = CompileFailureState{ result.sourceHash, false };
                }
            }
            compileRetryAfter_[result.filepath] = glfwGetTime() + COMPILE_FAILURE_RETRY_SECONDS;
            continue;
        }

        auto failureIt = compileFailures_.find(result.filepath);
        if (failureIt != compileFailures_.end() && failureIt->second.sourceHash == result.sourceHash) {
            compileFailures_.erase(failureIt);
        }
        compileRetryAfter_.erase(result.filepath);

        compiledPrograms_[result.filepath] = result.program;
        if (result.filepath == activeFilePath_) {
            activeProgram_ = result.program;
            if (activeProgram_->functions.init) {
                activeProgram_->functions.init(&ctx_);
            }
        }
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
        SubmitDueCompiles(now);
        ProcessCompileResults();

        ctx_.deltaTime = static_cast<float>(now - lastTime_);
        ctx_.time = static_cast<float>(now);
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

    if (watcher_) {
        watcher_->Stop();
        watcher_.reset();
    }

    compileThreadRunning_.store(false);
    compileCv_.notify_all();
    if (compileThread_.joinable()) {
        compileThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(compileMutex_);
        compileJobs_.clear();
        compileResults_ = std::queue<CompileResult>{};
        inFlightSourceHashes_.clear();
    }

    activeProgram_.reset();
    compiledPrograms_.clear();
    compileFailures_.clear();
    compileRetryAfter_.clear();

    if (jit_) {
        jit_->Terminate();
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