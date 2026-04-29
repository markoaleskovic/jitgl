// Available: init(), update(), renderFrame(), shutdown()
// EngineContext* ctx gives you: time, deltaTime, width, height, vao, vbo, defaultShader
// frameCount, reloadCount, state_i[64], state_f[64] survive hot-swaps.
// Tip: Use init() to compile shaders once and store IDs in state_i.

struct Mat4 {
    float m[16];
};

static Mat4 mat4_identity() {
    Mat4 r = {};
    r.m[0]  = 1.0f;
    r.m[5]  = 1.0f;
    r.m[10] = 1.0f;
    r.m[15] = 1.0f;
    return r;
}

static Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r.m[col * 4 + row] =
                a.m[0 * 4 + row] * b.m[col * 4 + 0] +
                a.m[1 * 4 + row] * b.m[col * 4 + 1] +
                a.m[2 * 4 + row] * b.m[col * 4 + 2] +
                a.m[3 * 4 + row] * b.m[col * 4 + 3];
        }
    }
    return r;
}

static Mat4 mat4_translate(float x, float y, float z) {
    Mat4 r = mat4_identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

static Mat4 mat4_rotate_x(float a) {
    Mat4 r = mat4_identity();
    float c = cosf(a);
    float s = sinf(a);

    r.m[5]  = c;
    r.m[6]  = s;
    r.m[9]  = -s;
    r.m[10] = c;
    return r;
}

static Mat4 mat4_rotate_y(float a) {
    Mat4 r = mat4_identity();
    float c = cosf(a);
    float s = sinf(a);

    r.m[0]  = c;
    r.m[2]  = -s;
    r.m[8]  = s;
    r.m[10] = c;
    return r;
}

static Mat4 mat4_perspective(float fovYRadians, float aspect, float zNear, float zFar) {
    Mat4 r = {};
    float f = 1.0f / tanf(fovYRadians * 0.5f);

    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zFar + zNear) / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
    return r;
}

// GLSL-like shaders
const char* vs = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;

    uniform mat4 uMVP;

    out vec3 vLocalPos;

    void main() {
        vLocalPos = aPos;
        gl_Position = uMVP * vec4(aPos, 1.0);
    }
)";

const char* fs = R"(
    #version 330 core
    in vec3 vLocalPos;
    out vec4 FragColor;

    uniform float time;

    void main() {
        vec3 base = 0.35 + 0.65 * abs(normalize(vLocalPos));
        float pulse = 0.80 + 0.18 * sin(time * 10.0);
        FragColor = vec4(base * pulse, 1.0);
    }
)";

extern "C" void init(EngineContext* ctx) {
    printf("[JIT] Initialize rotating cubes (reload #%u)\n", ctx->reloadCount);

    // Clean up previous hot-reload shader program if one exists.
    GLuint oldProgram = (GLuint)STATE_I(0);
    if (oldProgram != 0) {
        glDeleteProgram(oldProgram);
        STATE_I(0) = 0;
    }

    GLuint program = jit_link_program(
        jit_compile_shader(GL_VERTEX_SHADER, vs),
        jit_compile_shader(GL_FRAGMENT_SHADER, fs)
    );

    STATE_I(0) = (int)program;
    STATE_I(1) = (int)glGetUniformLocation(program, "uMVP");
    STATE_I(2) = (int)glGetUniformLocation(program, "time");

    // 36 vertices, 12 triangles, position only.
    static const float cubeVerts[] = {
        // back face
        -0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
         0.5f,  0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,

        // front face
        -0.5f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,
         0.5f,  0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,

        // left face
        -0.5f,  0.5f,  0.5f,  -0.5f,  0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,
        -0.5f, -0.5f, -0.5f,  -0.5f, -0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,

        // right face
         0.5f,  0.5f,  0.5f,   0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,   0.5f,  0.5f,  0.5f,   0.5f, -0.5f,  0.5f,

        // bottom face
        -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,
         0.5f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,  -0.5f, -0.5f, -0.5f,

        // top face
        -0.5f,  0.5f, -0.5f,   0.5f,  0.5f,  0.5f,   0.5f,  0.5f, -0.5f,
         0.5f,  0.5f,  0.5f,  -0.5f,  0.5f, -0.5f,  -0.5f,  0.5f,  0.5f
    };

    glBindVertexArray(ctx->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

extern "C" void update(EngineContext* ctx) {
    // You can move camera/game logic here later.
    // For now, rotation comes directly from ctx->time in renderFrame().
}

extern "C" void renderFrame(EngineContext* ctx) {
    GLuint program = (GLuint)STATE_I(0);
    GLint uMVPLoc  = (GLint)STATE_I(1);
    GLint uTimeLoc = (GLint)STATE_I(2);

    glViewport(0, 0, ctx->width, ctx->height);
    glEnable(GL_DEPTH_TEST);

    glClearColor(0.08f, 0.10f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(program);
    glBindVertexArray(ctx->vao);
    glUniform1f(uTimeLoc, ctx->time);

    float aspect = (ctx->height > 0) ? ((float)ctx->width / (float)ctx->height) : 1.0f;
    Mat4 proj = mat4_perspective(30.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);
    Mat4 view = mat4_translate(0.0f, 0.0f, -6.0f);
    Mat4 vp   = mat4_mul(proj, view);

    // Cube 1: centered left, spinning in place.
    Mat4 rot1   = mat4_mul(mat4_rotate_y(ctx->time * 9.10f), mat4_rotate_x(ctx->time * 0.70f));
    Mat4 model1 = mat4_mul(mat4_translate(-1.6f, 0.0f, 0.0f), rot1);
    Mat4 mvp1   = mat4_mul(vp, model1);

    glUniformMatrix4fv(uMVPLoc, 1, GL_FALSE, mvp1.m);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    // Cube 2: orbiting-ish motion with a different spin.
    Mat4 orbit  = mat4_translate(cosf(ctx->time) * 2.2f, sinf(ctx->time * 0.8f) * 0.7f, -1.0f);
    Mat4 rot2   = mat4_mul(mat4_rotate_y(-ctx->time * 1.6f), mat4_rotate_x(ctx->time * 1.2f));
    Mat4 model2 = mat4_mul(orbit, rot2);
    Mat4 mvp2   = mat4_mul(vp, model2);

    glUniformMatrix4fv(uMVPLoc, 1, GL_FALSE, mvp2.m);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    glBindVertexArray(0);
}

extern "C" void shutdown(EngineContext* ctx) {
    printf("[JIT] Shutdown scene (frame %llu)\n", ctx->frameCount);

    GLuint program = (GLuint)STATE_I(0);
    if (program != 0) {
        glDeleteProgram(program);
        STATE_I(0) = 0;
    }
}

