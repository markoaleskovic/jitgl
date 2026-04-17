#include "core/Engine.h"
#include "ui/EditorUI.h"
#include "watcher/FileWatcher.h"
#include "system/ConsoleRedirectSession.h"
#include "core/WorkspaceManager.h" // Add this include
#include "runtime/EngineContext.h"
#include "jit/JitEngine.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <streambuf>
#include <fstream>

namespace {
    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr const char* WINDOW_TITLE = "JITGL";

    constexpr int GL_VERSION_MAJOR = 3;
    constexpr int GL_VERSION_MINOR = 3;

    constexpr const char* WORKSPACE_DIR = "workspace";
    constexpr const char* PREAMBLE_PATH = "workspace/engine.hpp";
}


// ── Engine ───────────────────────────────────────────────────────────────────
Engine::Engine() = default;
Engine::~Engine() { Shutdown(); }

bool Engine::Init() {
    return InitWindow() && InitGL() && InitUI() && InitJIT() && InitWatcher();
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

bool Engine::InitGL() {
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::cerr << "GLAD init failed\n";
        return false;
    }

    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    ctx_.width  = w;
    ctx_.height = h;

    float quadVerts[] = {
        -1.f, -1.f,   1.f, -1.f,   1.f,  1.f,
        -1.f, -1.f,   1.f,  1.f,  -1.f,  1.f,
    };
    glGenVertexArrays(1, &ctx_.vao);
    glGenBuffers(1, &ctx_.vbo);
    glBindVertexArray(ctx_.vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

    return true;
}

bool Engine::InitUI() {
    ui_ = std::make_unique<EditorUI>(); // Fixed memory management
    ui_->Init(window_);

    consoleRedirect_ = std::make_unique<ConsoleRedirectSession>();
    if (!consoleRedirect_->Start(ui_.get())) { // Pass raw pointer to redirector
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

    const std::string preamblePath = "workspace/engine.hpp";
    if (!jit_->Init(preamblePath)) {
        ui_->AddLogOutput("[Engine] JIT engine failed to initialize.");
        return false;
    }

    return true;
}

bool Engine::InitWatcher() {
    // 1. Initialize WorkspaceManager (creates dir and main.cpp if needed)
    workspaceManager_ = std::make_unique<WorkspaceManager>(WORKSPACE_DIR);
    workspaceManager_->Initialize();

    // 2. Load files into UI
    auto files = workspaceManager_->LoadAllFiles();
    for (const auto& f : files) {
        ui_->AddDocument(f.filename, f.filepath, f.content);
    }

    // 3. Bind UI save events to WorkspaceManager
    ui_->SetSaveCallback([this](const std::string& path, const std::string& content) {
        return workspaceManager_->SaveFile(path, content);
    });

    // 4. Start file watcher
    watcher_ = std::make_unique<FileWatcher>(
        WORKSPACE_DIR,
        [this](const std::string& path) { OnFileChanged(path); }
    );
    watcher_->Start();

    lastTime_ = glfwGetTime();

    ui_->AddLogOutput("[Engine] Initialized successfully.");
    return true;
}

void Engine::OnFileChanged(const std::string& filepath) {
    if (filepath.find(".cpp") == std::string::npos) return;

    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingReloads_.push(filepath);
}

void Engine::ProcessPendingReloads() {
    // 1. Drain the entire queue into a local vector to minimize lock time
    std::vector<std::string> filesToProcess;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingReloads_.empty()) return;

        while (!pendingReloads_.empty()) {
            filesToProcess.push_back(pendingReloads_.front());
            pendingReloads_.pop();
        }
    }

    // 2. Process all gathered files outside the lock
    for (const auto& filepath : filesToProcess) {
        ui_->AddLogOutput("[Watcher] Change detected: " + filepath);

        bool ok = jit_->ReloadFile(filepath);
        if (ok) {
            auto fn = jit_->GetFunctions();
            // Call init() only if we successfully acquired new valid pointers
            if (fn.init) {
                fn.init(&ctx_);
                ui_->AddLogOutput("[JIT] init() called.");
            }
        }
    }
}

void Engine::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        ProcessPendingReloads();

        double now     = glfwGetTime();
        ctx_.deltaTime = static_cast<float>(now - lastTime_);
        ctx_.time      = static_cast<float>(now);
        lastTime_      = now;

        int w, h;
        glfwGetFramebufferSize(window_, &w, &h);
        ctx_.width  = w;
        ctx_.height = h;
        glViewport(0, 0, w, h);

        ui_->NewFrame();
        ui_->Draw();

        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        auto fn = jit_->GetFunctions();
        if (fn.update) fn.update(&ctx_);
        if (fn.render) fn.render(&ctx_);

        ui_->Render();
        glfwSwapBuffers(window_);
    }
}

void Engine::Shutdown() {
    if (watcher_) {
        watcher_->Stop();
        watcher_.reset();
    }

    if (jit_) {
        jit_.reset();
    }

    if (consoleRedirect_) {
        consoleRedirect_->Stop();
        consoleRedirect_.reset();
    }

    if (workspaceManager_) {
        workspaceManager_.reset();
    }

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
        ui_.reset(); // Fixed memory management
    }

    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    glfwTerminate();
}