// JITGL Input Example
//
// Drives a small white square around the renderer with WASD and shows the
// mouse cursor as a faint cross. Spacebar toggles a color tint.
//
// To try it:
//   1. Create or replace a workspace's scene.cpp with this file.
//   2. Tick "Capture Input" above the renderer (or press F1).
//   3. Click the renderer to give it focus, then mash WASD.
//
// STATE map:
//   STATE_F(0) = quad center X in NDC ([-1, 1])
//   STATE_F(1) = quad center Y in NDC ([-1, 1])
//   STATE_F(2) = tint hue in [0, 1)
//   STATE_I(0) = shader program
//   STATE_I(1) = uniform location for uCenter
//   STATE_I(2) = uniform location for uTint
//   STATE_I(3) = uniform location for uMouse
//   STATE_I(4) = uniform location for uSize
//   STATE_I(5) = cached shader hash

extern "C" void init(EngineContext* ctx) {
    (void)jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH);

    GLuint program = static_cast<GLuint>(STATE_I(0));
    const uint32_t cachedHash = STATE_I(5);
    const uint32_t builtInHash = static_cast<uint32_t>(
        jit_fnv1a64("jitgl-input-example-v1") & 0xFFFFFFFFu);

    if (program == 0 || cachedHash != builtInHash) {
        if (program != 0) {
            glDeleteProgram(program);
        }

        const char* vs_src =
            "#version 330 core\n"
            "layout(location=0) in vec2 aPos;\n"
            "uniform vec2 uCenter;\n"
            "uniform float uSize;\n"
            "out vec2 vUv;\n"
            "void main() {\n"
            "    vUv = aPos;\n"
            "    gl_Position = vec4(aPos * uSize + uCenter, 0.0, 1.0);\n"
            "}\n";

        const char* fs_src =
            "#version 330 core\n"
            "in vec2 vUv;\n"
            "uniform vec3 uTint;\n"
            "uniform vec2 uMouse;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    float d = length(vUv);\n"
            "    float core = smoothstep(1.0, 0.4, d);\n"
            "    fragColor = vec4(uTint * core, 1.0);\n"
            "    if (length(uMouse) > 0.0001) {\n"
            "        // brighten near mouse to make the cursor visible too\n"
            "        float m = exp(-12.0 * length(vUv - uMouse));\n"
            "        fragColor.rgb += vec3(m);\n"
            "    }\n"
            "}\n";

        GLuint vs = jit_compile_shader(GL_VERTEX_SHADER, vs_src);
        GLuint fs = jit_compile_shader(GL_FRAGMENT_SHADER, fs_src);
        program = jit_link_program(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        STATE_I(0) = static_cast<uint32_t>(program);
        STATE_I(1) = static_cast<uint32_t>(glGetUniformLocation(program, "uCenter"));
        STATE_I(2) = static_cast<uint32_t>(glGetUniformLocation(program, "uTint"));
        STATE_I(3) = static_cast<uint32_t>(glGetUniformLocation(program, "uMouse"));
        STATE_I(4) = static_cast<uint32_t>(glGetUniformLocation(program, "uSize"));
        STATE_I(5) = builtInHash;
    }
}

extern "C" void update(EngineContext* ctx) {
    const float speed = 1.5f * ctx->deltaTime;
    if (KEY_DOWN(ctx, JITGL_KEY_W)) STATE_F(1) += speed;
    if (KEY_DOWN(ctx, JITGL_KEY_S)) STATE_F(1) -= speed;
    if (KEY_DOWN(ctx, JITGL_KEY_A)) STATE_F(0) -= speed;
    if (KEY_DOWN(ctx, JITGL_KEY_D)) STATE_F(0) += speed;

    // Clamp the quad so it stays visible.
    if (STATE_F(0) < -0.9f) STATE_F(0) = -0.9f;
    if (STATE_F(0) >  0.9f) STATE_F(0) =  0.9f;
    if (STATE_F(1) < -0.9f) STATE_F(1) = -0.9f;
    if (STATE_F(1) >  0.9f) STATE_F(1) =  0.9f;

    if (KEY_PRESSED(ctx, JITGL_KEY_SPACE)) {
        STATE_F(2) += 0.17f;
        if (STATE_F(2) > 1.0f) STATE_F(2) -= 1.0f;
    }
    if (KEY_PRESSED(ctx, JITGL_KEY_R)) {
        STATE_F(0) = 0.0f;
        STATE_F(1) = 0.0f;
        STATE_F(2) = 0.0f;
    }
}

static void hueToRgb(float h, float* r, float* g, float* b) {
    const float k = h * 6.0f;
    const float c = 1.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(k, 2.0f) - 1.0f));
    if      (k < 1.0f) { *r = c; *g = x; *b = 0; }
    else if (k < 2.0f) { *r = x; *g = c; *b = 0; }
    else if (k < 3.0f) { *r = 0; *g = c; *b = x; }
    else if (k < 4.0f) { *r = 0; *g = x; *b = c; }
    else if (k < 5.0f) { *r = x; *g = 0; *b = c; }
    else               { *r = c; *g = 0; *b = x; }
}

extern "C" void renderFrame(EngineContext* ctx) {
    const GLuint program = static_cast<GLuint>(STATE_I(0));
    if (program == 0) return;

    glViewport(0, 0, ctx->width, ctx->height);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program);
    glUniform2f(static_cast<GLint>(STATE_I(1)), STATE_F(0), STATE_F(1));
    float r = 1.0f, g = 1.0f, b = 1.0f;
    hueToRgb(STATE_F(2), &r, &g, &b);
    glUniform3f(static_cast<GLint>(STATE_I(2)), r, g, b);
    if (MOUSE_IN_VIEWPORT(ctx)) {
        glUniform2f(static_cast<GLint>(STATE_I(3)),
                    ctx->input.mouseNdcX - STATE_F(0),
                    ctx->input.mouseNdcY - STATE_F(1));
    } else {
        glUniform2f(static_cast<GLint>(STATE_I(3)), 0.0f, 0.0f);
    }
    glUniform1f(static_cast<GLint>(STATE_I(4)), 0.12f);

    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

extern "C" void shutdown(EngineContext* ctx) {
    if (const GLuint program = static_cast<GLuint>(STATE_I(0)); program != 0) {
        glDeleteProgram(program);
        STATE_I(0) = 0;
    }
}
