// Empty fragment shader workspace.
//
// Bare minimum: one uniform-driven solid color over a fullscreen quad. Use
// this as a starting point for fragment-only experiments (post-process
// effects, raymarching, etc). The host already gives you a 6-vertex
// fullscreen triangle pair at ctx->vao — no geometry setup needed.

extern "C" void init(EngineContext* ctx) {
    (void)jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH);

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
        STATE_I(1) = static_cast<uint32_t>(glGetUniformLocation(program, "uColor"));
        STATE_I(3) = JIT_WORKSPACE_SHADER_HASH;
    }
}

extern "C" void update(EngineContext* ctx) { (void)ctx; }
extern "C" void dispatchCompute(EngineContext* ctx) { (void)ctx; }

extern "C" void renderFrame(EngineContext* ctx) {
    glViewport(0, 0, ctx->width, ctx->height);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const GLuint program = static_cast<GLuint>(STATE_I(0));
    if (program == 0) return;
    glUseProgram(program);
    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

extern "C" void shutdown(EngineContext* ctx) {
    GLuint program = static_cast<GLuint>(STATE_I(0));
    if (program != 0) {
        glDeleteProgram(program);
        STATE_I(0) = 0;
    }
}
