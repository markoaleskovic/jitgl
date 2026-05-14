// JITGL Showcase Workspace
// This scene compiles the current shader.glsl sections and draws a fullscreen
// quad so uniform controls and live shader edits are immediately visible.

extern "C" void init(EngineContext* ctx) {
    (void)jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH);

    GLuint currentProgram = static_cast<GLuint>(STATE_I(0));
    const uint32_t cachedShaderHash = STATE_I(3);

    if (currentProgram == 0 || cachedShaderHash != JIT_WORKSPACE_SHADER_HASH) {
        if (currentProgram != 0) {
            glDeleteProgram(currentProgram);
        }

        GLuint vs = jit_compile_shader(GL_VERTEX_SHADER, JIT_WORKSPACE_VERTEX_SHADER);
        GLuint fs = jit_compile_shader(GL_FRAGMENT_SHADER, JIT_WORKSPACE_FRAGMENT_SHADER);
        GLuint program = jit_link_program(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        STATE_I(0) = static_cast<uint32_t>(program);
        STATE_I(1) = static_cast<uint32_t>(glGetUniformLocation(program, "uTime"));
        STATE_I(2) = static_cast<uint32_t>(glGetUniformLocation(program, "uResolution"));
        STATE_I(3) = JIT_WORKSPACE_SHADER_HASH;

        std::printf("[Showcase] Shader program rebuilt (hash=%u)\n", JIT_WORKSPACE_SHADER_HASH);
    }
}

extern "C" void update(EngineContext* ctx) {
    (void)ctx;
}

extern "C" void renderFrame(EngineContext* ctx) {
    const GLuint program = static_cast<GLuint>(STATE_I(0));
    const GLint uTime = static_cast<GLint>(STATE_I(1));
    const GLint uResolution = static_cast<GLint>(STATE_I(2));

    glViewport(0, 0, ctx->width, ctx->height);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.015f, 0.015f, 0.025f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (program == 0) {
        return;
    }

    glUseProgram(program);
    if (uTime >= 0) {
        glUniform1f(uTime, ctx->time);
    }
    if (uResolution >= 0) {
        glUniform2f(uResolution, static_cast<float>(ctx->width), static_cast<float>(ctx->height));
    }

    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

extern "C" void shutdown(EngineContext* ctx) {
    (void)ctx;
}
