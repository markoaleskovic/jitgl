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
        file << "// Available: init(), update(), renderFrame(), shutdown()\n"
                "// EngineContext* ctx gives you: time, deltaTime, width, height, vao, vbo, defaultShader\n"
                "// frameCount, reloadCount, state_i[64], state_f[64] survive hot-swaps.\n"
                "// Tip: Use init() to compile shaders once and store IDs in state_i.\n\n"
                "// GLSL-like shaders\n"
                "const char* vs = R\"(\n"
                "    #version 330 core\n"
                "    layout (location = 0) in vec2 aPos;\n"
                "    void main() {\n"
                "        gl_Position = vec4(aPos, 0.0, 1.0);\n"
                "    }\n"
                ")\";\n\n"
                "const char* fs = R\"(\n"
                "    #version 330 core\n"
                "    out vec4 FragColor;\n"
                "    uniform float time;\n"
                "    void main() {\n"
                "        // Write your shader logic here!\n"
                "        vec2 uv = gl_FragCoord.xy / vec2(1280, 720); // roughly\n"
                "        FragColor = vec4(uv.x, uv.y, 0.5 + 0.5*sin(time), 1.0);\n"
                "    }\n"
                ")\";\n\n"
                "extern \"C\" void init(EngineContext* ctx) {\n"
                "    printf(\"[JIT] Initialize scene (reload #%u)\\n\", ctx->reloadCount);\n"
                "    \n"
                "    // Compile custom shader\n"
                "    GLuint program = jit_link_program(jit_compile_shader(GL_VERTEX_SHADER, vs), \n"
                "                                      jit_compile_shader(GL_FRAGMENT_SHADER, fs));\n"
                "    STATE_I(0) = (int)program; // Store in persistent state\n"
                "}\n\n"
                "extern \"C\" void update(EngineContext* ctx) {\n"
                "    // Logic goes here (runs every frame)\n"
                "}\n\n"
                "extern \"C\" void renderFrame(EngineContext* ctx) {\n"
                "    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);\n"
                "    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n"
                "    \n"
                "    GLuint program = (GLuint)STATE_I(0);\n"
                "    if (program != 0) {\n"
                "        glUseProgram(program);\n"
                "        glUniform1f(glGetUniformLocation(program, \"time\"), ctx->time);\n"
                "        glBindVertexArray(ctx->vao);\n"
                "        glDrawArrays(GL_TRIANGLES, 0, 6);\n"
                "    } else {\n"
                "        // Fallback to default shader if custom one failed\n"
                "        glUseProgram(ctx->defaultShader);\n"
                "        glUniform1f(glGetUniformLocation(ctx->defaultShader, \"time\"), ctx->time);\n"
                "        glBindVertexArray(ctx->vao);\n"
                "        glDrawArrays(GL_TRIANGLES, 0, 6);\n"
                "    }\n"
                "}\n\n"
                "extern \"C\" void shutdown(EngineContext* ctx) {\n"
                "    printf(\"[JIT] Shutdown scene (frame %llu)\\n\", ctx->frameCount);\n"
                "    GLuint program = (GLuint)STATE_I(0);\n"
                "    if (program != 0) glDeleteProgram(program);\n"
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
