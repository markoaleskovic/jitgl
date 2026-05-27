// Camera + cube.
//
// Demonstrates the opt-in 3D camera helper. The host provides view and
// projection matrices in ctx->camera once you set `enabled = 1`. Orbit
// controls: left-drag rotates, right-drag pans, scroll zooms. Switch to
// free-fly by setting `ctx->camera.mode = 1` and toggle input capture (F1)
// for WASD + QE movement.
//
// Geometry is a hand-rolled cube built once at init() and stashed on
// STATE_I. Each face is shaded by its normal so the orientation is always
// readable.

namespace {
    // 36 vertices, 6 floats each (xyz position + xyz normal). Faces wound
    // CCW with the implicit normal pointing outward.
    constexpr float kCubeVerts[] = {
        // +X face (normal 1,0,0)
         0.5f,-0.5f,-0.5f,  1, 0, 0,
         0.5f, 0.5f,-0.5f,  1, 0, 0,
         0.5f, 0.5f, 0.5f,  1, 0, 0,
         0.5f,-0.5f,-0.5f,  1, 0, 0,
         0.5f, 0.5f, 0.5f,  1, 0, 0,
         0.5f,-0.5f, 0.5f,  1, 0, 0,
        // -X face
        -0.5f,-0.5f, 0.5f, -1, 0, 0,
        -0.5f, 0.5f, 0.5f, -1, 0, 0,
        -0.5f, 0.5f,-0.5f, -1, 0, 0,
        -0.5f,-0.5f, 0.5f, -1, 0, 0,
        -0.5f, 0.5f,-0.5f, -1, 0, 0,
        -0.5f,-0.5f,-0.5f, -1, 0, 0,
        // +Y face
        -0.5f, 0.5f,-0.5f,  0, 1, 0,
        -0.5f, 0.5f, 0.5f,  0, 1, 0,
         0.5f, 0.5f, 0.5f,  0, 1, 0,
        -0.5f, 0.5f,-0.5f,  0, 1, 0,
         0.5f, 0.5f, 0.5f,  0, 1, 0,
         0.5f, 0.5f,-0.5f,  0, 1, 0,
        // -Y face
        -0.5f,-0.5f, 0.5f,  0,-1, 0,
        -0.5f,-0.5f,-0.5f,  0,-1, 0,
         0.5f,-0.5f,-0.5f,  0,-1, 0,
        -0.5f,-0.5f, 0.5f,  0,-1, 0,
         0.5f,-0.5f,-0.5f,  0,-1, 0,
         0.5f,-0.5f, 0.5f,  0,-1, 0,
        // +Z face
         0.5f,-0.5f, 0.5f,  0, 0, 1,
         0.5f, 0.5f, 0.5f,  0, 0, 1,
        -0.5f, 0.5f, 0.5f,  0, 0, 1,
         0.5f,-0.5f, 0.5f,  0, 0, 1,
        -0.5f, 0.5f, 0.5f,  0, 0, 1,
        -0.5f,-0.5f, 0.5f,  0, 0, 1,
        // -Z face
        -0.5f,-0.5f,-0.5f,  0, 0,-1,
        -0.5f, 0.5f,-0.5f,  0, 0,-1,
         0.5f, 0.5f,-0.5f,  0, 0,-1,
        -0.5f,-0.5f,-0.5f,  0, 0,-1,
         0.5f, 0.5f,-0.5f,  0, 0,-1,
         0.5f,-0.5f,-0.5f,  0, 0,-1,
    };
    constexpr int kCubeVertexCount = 36;
}

extern "C" void init(EngineContext* ctx) {
    (void)jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH);

    // Opt the camera in. Defaults give a sensible orbit around the origin.
    ctx->camera.enabled = 1;
    ctx->camera.mode = 0;            // orbit
    ctx->camera.target[0] = 0.0f;
    ctx->camera.target[1] = 0.0f;
    ctx->camera.target[2] = 0.0f;
    if (ctx->camera.distance < 0.1f) ctx->camera.distance = 4.0f;

    GLuint program = static_cast<GLuint>(STATE_I(0));
    const uint32_t cachedHash = STATE_I(3);
    if (program == 0 || cachedHash != JIT_WORKSPACE_SHADER_HASH) {
        if (program != 0) glDeleteProgram(program);
        GLuint vs = jit_compile_shader(GL_VERTEX_SHADER, JIT_WORKSPACE_VERTEX_SHADER);
        GLuint fs = jit_compile_shader(GL_FRAGMENT_SHADER, JIT_WORKSPACE_FRAGMENT_SHADER);
        program = jit_link_program(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        STATE_I(0) = static_cast<uint32_t>(program);
        STATE_I(1) = static_cast<uint32_t>(glGetUniformLocation(program, "uViewProj"));
        STATE_I(2) = static_cast<uint32_t>(glGetUniformLocation(program, "uModel"));
        STATE_I(3) = JIT_WORKSPACE_SHADER_HASH;
    }

    // Build a cube VAO/VBO. The host's ctx->vao is the fullscreen quad
    // and isn't suitable here; we own a separate pair.
    GLuint vao = static_cast<GLuint>(STATE_I(10));
    GLuint vbo = static_cast<GLuint>(STATE_I(11));
    if (vao == 0 || glIsVertexArray(vao) == GL_FALSE) {
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVerts), kCubeVerts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
        STATE_I(10) = static_cast<uint32_t>(vao);
        STATE_I(11) = static_cast<uint32_t>(vbo);
    }
}

extern "C" void update(EngineContext* ctx) { (void)ctx; }
extern "C" void dispatchCompute(EngineContext* ctx) { (void)ctx; }

extern "C" void renderFrame(EngineContext* ctx) {
    glViewport(0, 0, ctx->width, ctx->height);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const GLuint program = static_cast<GLuint>(STATE_I(0));
    const GLuint vao = static_cast<GLuint>(STATE_I(10));
    if (program == 0 || vao == 0) return;

    glUseProgram(program);

    const GLint uViewProj = static_cast<GLint>(STATE_I(1));
    const GLint uModel = static_cast<GLint>(STATE_I(2));
    if (uViewProj >= 0) {
        glUniformMatrix4fv(uViewProj, 1, GL_FALSE, ctx->camera.view_projection);
    }
    // Slow auto-rotation around Y to give the cube some motion even when
    // the user isn't dragging the camera. Column-major rotY matrix.
    const float a = ctx->time * 0.6f;
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    const float model[16] = {
         ca, 0,-sa, 0,
          0, 1,  0, 0,
         sa, 0, ca, 0,
          0, 0,  0, 1,
    };
    if (uModel >= 0) {
        glUniformMatrix4fv(uModel, 1, GL_FALSE, model);
    }

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, kCubeVertexCount);
    glBindVertexArray(0);
}

extern "C" void shutdown(EngineContext* ctx) {
    GLuint vao = static_cast<GLuint>(STATE_I(10));
    GLuint vbo = static_cast<GLuint>(STATE_I(11));
    if (vao != 0) glDeleteVertexArrays(1, &vao);
    if (vbo != 0) glDeleteBuffers(1, &vbo);
    GLuint program = static_cast<GLuint>(STATE_I(0));
    if (program != 0) glDeleteProgram(program);
    STATE_I(0) = 0;
    STATE_I(10) = 0;
    STATE_I(11) = 0;
}
