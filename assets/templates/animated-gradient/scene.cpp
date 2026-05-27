// Animated gradient.
//
// A clean introduction to the live-edit loop. The fragment shader does all
// the work — `uTime` from the host drives a smooth color sweep, and the
// uniform controls in the Uniforms panel let you tweak the look while
// playback runs.

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
        STATE_I(1) = static_cast<uint32_t>(glGetUniformLocation(program, "uTime"));
        STATE_I(2) = static_cast<uint32_t>(glGetUniformLocation(program, "uResolution"));
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

    const GLint uTime = static_cast<GLint>(STATE_I(1));
    const GLint uResolution = static_cast<GLint>(STATE_I(2));

    glUseProgram(program);
    if (uTime >= 0) glUniform1f(uTime, ctx->time);
    if (uResolution >= 0) glUniform2f(uResolution,
                                       static_cast<float>(ctx->width),
                                       static_cast<float>(ctx->height));

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
