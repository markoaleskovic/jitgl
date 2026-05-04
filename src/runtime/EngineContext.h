#pragma once
#include <glad/gl.h>

// Plain data, owned by Engine, passed into every JIT callback.
// Never delete or store this pointer -- the host manages its lifetime.
struct EngineContext {
    float    time        = 0.0f;  // active seconds for current workspace (paused while inactive)
    float    deltaTime   = 0.0f;  // seconds since last frame
    uint64_t frameCount  = 0;     // total frames rendered
    uint32_t reloadCount = 0;     // total hot-swaps performed
    int      width       = 0;     // viewport width
    int      height      = 0;     // viewport height
    GLuint   defaultShader = 0;   // host-compiled fallback shader
    GLuint   vao         = 0;     // pre-allocated geometry VAO
    GLuint   vbo         = 0;     // pre-allocated geometry VBO

    // Persistent state that survives JIT hot-swaps.
    // Use these to store OpenGL resource IDs (textures, buffers, etc.)
    // or any other state you want to keep across code edits.
    uint32_t state_i[64] = {0};   // Increased to 64
    float    state_f[64] = {0.0f}; // Increased to 64
    void*    userData    = nullptr;
};
