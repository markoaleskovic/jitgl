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
        "struct Mat4 {\n"
        "    float m[16];\n"
        "};\n\n"
        "static Mat4 mat4_identity() {\n"
        "    Mat4 r = {};\n"
        "    r.m[0]  = 1.0f;\n"
        "    r.m[5]  = 1.0f;\n"
        "    r.m[10] = 1.0f;\n"
        "    r.m[15] = 1.0f;\n"
        "    return r;\n"
        "}\n\n"
        "static Mat4 mat4_mul(const Mat4& a, const Mat4& b) {\n"
        "    Mat4 r = {};\n"
        "    for (int row = 0; row < 4; ++row) {\n"
        "        for (int col = 0; col < 4; ++col) {\n"
        "            r.m[col * 4 + row] =\n"
        "                a.m[0 * 4 + row] * b.m[col * 4 + 0] +\n"
        "                a.m[1 * 4 + row] * b.m[col * 4 + 1] +\n"
        "                a.m[2 * 4 + row] * b.m[col * 4 + 2] +\n"
        "                a.m[3 * 4 + row] * b.m[col * 4 + 3];\n"
        "        }\n"
        "    }\n"
        "    return r;\n"
        "}\n\n"
        "static Mat4 mat4_translate(float x, float y, float z) {\n"
        "    Mat4 r = mat4_identity();\n"
        "    r.m[12] = x;\n"
        "    r.m[13] = y;\n"
        "    r.m[14] = z;\n"
        "    return r;\n"
        "}\n\n"
        "static Mat4 mat4_rotate_x(float a) {\n"
        "    Mat4 r = mat4_identity();\n"
        "    float c = cosf(a);\n"
        "    float s = sinf(a);\n\n"
        "    r.m[5]  = c;\n"
        "    r.m[6]  = s;\n"
        "    r.m[9]  = -s;\n"
        "    r.m[10] = c;\n"
        "    return r;\n"
        "}\n\n"
        "static Mat4 mat4_rotate_y(float a) {\n"
        "    Mat4 r = mat4_identity();\n"
        "    float c = cosf(a);\n"
        "    float s = sinf(a);\n\n"
        "    r.m[0]  = c;\n"
        "    r.m[2]  = -s;\n"
        "    r.m[8]  = s;\n"
        "    r.m[10] = c;\n"
        "    return r;\n"
        "}\n\n"
        "static Mat4 mat4_perspective(float fovYRadians, float aspect, float zNear, float zFar) {\n"
        "    Mat4 r = {};\n"
        "    float f = 1.0f / tanf(fovYRadians * 0.5f);\n\n"
        "    r.m[0]  = f / aspect;\n"
        "    r.m[5]  = f;\n"
        "    r.m[10] = (zFar + zNear) / (zNear - zFar);\n"
        "    r.m[11] = -1.0f;\n"
        "    r.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);\n"
        "    return r;\n"
        "}\n\n"
        "// GLSL-like shaders\n"
        "const char* vs = R\"(\n"
        "    #version 330 core\n"
        "    layout (location = 0) in vec3 aPos;\n\n"
        "    uniform mat4 uMVP;\n\n"
        "    out vec3 vLocalPos;\n\n"
        "    void main() {\n"
        "        vLocalPos = aPos;\n"
        "        gl_Position = uMVP * vec4(aPos, 1.0);\n"
        "    }\n"
        ")\";\n\n"
        "const char* fs = R\"(\n"
        "    #version 330 core\n"
        "    in vec3 vLocalPos;\n"
        "    out vec4 FragColor;\n\n"
        "    uniform float time;\n\n"
        "    void main() {\n"
        "        vec3 base = 0.35 + 0.65 * abs(normalize(vLocalPos));\n"
        "        float pulse = 0.82 + 0.18 * sin(time * 2.0);\n"
        "        FragColor = vec4(base * pulse, 1.0);\n"
        "    }\n"
        ")\";\n\n"
        "extern \"C\" void init(EngineContext* ctx) {\n"
        "    printf(\"[JIT] Initialize rotating cubes (reload #%u)\\n\", ctx->reloadCount);\n\n"
        "    // Clean up previous hot-reload shader program if one exists.\n"
        "    GLuint oldProgram = (GLuint)STATE_I(0);\n"
        "    if (oldProgram != 0) {\n"
        "        glDeleteProgram(oldProgram);\n"
        "        STATE_I(0) = 0;\n"
        "    }\n\n"
        "    GLuint program = jit_link_program(\n"
        "        jit_compile_shader(GL_VERTEX_SHADER, vs),\n"
        "        jit_compile_shader(GL_FRAGMENT_SHADER, fs)\n"
        "    );\n\n"
        "    STATE_I(0) = (int)program;\n"
        "    STATE_I(1) = (int)glGetUniformLocation(program, \"uMVP\");\n"
        "    STATE_I(2) = (int)glGetUniformLocation(program, \"time\");\n\n"
        "    // 36 vertices, 12 triangles, position only.\n"
        "    static const float cubeVerts[] = {\n"
        "        // back face\n"
        "        -0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,   0.5f, -0.5f, -0.5f,\n"
        "         0.5f,  0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,\n\n"
        "        // front face\n"
        "        -0.5f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,\n"
        "         0.5f,  0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,\n\n"
        "        // left face\n"
        "        -0.5f,  0.5f,  0.5f,  -0.5f,  0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,\n"
        "        -0.5f, -0.5f, -0.5f,  -0.5f, -0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,\n\n"
        "        // right face\n"
        "         0.5f,  0.5f,  0.5f,   0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,\n"
        "         0.5f, -0.5f, -0.5f,   0.5f,  0.5f,  0.5f,   0.5f, -0.5f,  0.5f,\n\n"
        "        // bottom face\n"
        "        -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,\n"
        "         0.5f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,  -0.5f, -0.5f, -0.5f,\n\n"
        "        // top face\n"
        "        -0.5f,  0.5f, -0.5f,   0.5f,  0.5f,  0.5f,   0.5f,  0.5f, -0.5f,\n"
        "         0.5f,  0.5f,  0.5f,  -0.5f,  0.5f, -0.5f,  -0.5f,  0.5f,  0.5f\n"
        "    };\n\n"
        "    glBindVertexArray(ctx->vao);\n"
        "    glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);\n"
        "    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);\n\n"
        "    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);\n"
        "    glEnableVertexAttribArray(0);\n\n"
        "    glBindBuffer(GL_ARRAY_BUFFER, 0);\n"
        "    glBindVertexArray(0);\n\n"
        "    glEnable(GL_DEPTH_TEST);\n"
        "}\n\n"
        "extern \"C\" void update(EngineContext* ctx) {\n"
        "    // You can move camera/game logic here later.\n"
        "    // For now, rotation comes directly from ctx->time in renderFrame().\n"
        "}\n\n"
        "extern \"C\" void renderFrame(EngineContext* ctx) {\n"
        "    GLuint program = (GLuint)STATE_I(0);\n"
        "    GLint uMVPLoc  = (GLint)STATE_I(1);\n"
        "    GLint uTimeLoc = (GLint)STATE_I(2);\n\n"
        "    glViewport(0, 0, ctx->width, ctx->height);\n"
        "    glEnable(GL_DEPTH_TEST);\n\n"
        "    glClearColor(0.08f, 0.10f, 0.14f, 1.0f);\n"
        "    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n\n"
        "    glUseProgram(program);\n"
        "    glBindVertexArray(ctx->vao);\n"
        "    glUniform1f(uTimeLoc, ctx->time);\n\n"
        "    float aspect = (ctx->height > 0) ? ((float)ctx->width / (float)ctx->height) : 1.0f;\n"
        "    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);\n"
        "    Mat4 view = mat4_translate(0.0f, 0.0f, -6.0f);\n"
        "    Mat4 vp   = mat4_mul(proj, view);\n\n"
        "    // Cube 1: centered left, spinning in place.\n"
        "    Mat4 rot1   = mat4_mul(mat4_rotate_y(ctx->time * 1.10f), mat4_rotate_x(ctx->time * 0.70f));\n"
        "    Mat4 model1 = mat4_mul(mat4_translate(-1.6f, 0.0f, 0.0f), rot1);\n"
        "    Mat4 mvp1   = mat4_mul(vp, model1);\n\n"
        "    glUniformMatrix4fv(uMVPLoc, 1, GL_FALSE, mvp1.m);\n"
        "    glDrawArrays(GL_TRIANGLES, 0, 36);\n\n"
        "    // Cube 2: orbiting-ish motion with a different spin.\n"
        "    Mat4 orbit  = mat4_translate(cosf(ctx->time) * 2.2f, sinf(ctx->time * 0.8f) * 0.7f, -1.0f);\n"
        "    Mat4 rot2   = mat4_mul(mat4_rotate_y(-ctx->time * 1.6f), mat4_rotate_x(ctx->time * 1.2f));\n"
        "    Mat4 model2 = mat4_mul(orbit, rot2);\n"
        "    Mat4 mvp2   = mat4_mul(vp, model2);\n\n"
        "    glUniformMatrix4fv(uMVPLoc, 1, GL_FALSE, mvp2.m);\n"
        "    glDrawArrays(GL_TRIANGLES, 0, 36);\n\n"
        "    glBindVertexArray(0);\n"
        "}\n\n"
        "extern \"C\" void shutdown(EngineContext* ctx) {\n"
        "    printf(\"[JIT] Shutdown scene (frame %llu)\\n\", ctx->frameCount);\n\n"
        "    GLuint program = (GLuint)STATE_I(0);\n"
        "    if (program != 0) {\n"
        "        glDeleteProgram(program);\n"
        "        STATE_I(0) = 0;\n"
        "    }\n"
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
