#pragma once
#include <glad/gl.h>

// Plain data, owned by Engine, passed into every JIT callback.
// Never delete or store this pointer -- the host manages its lifetime.
struct EngineContext {
    float  time        = 0.0f;  // seconds since start
    float  deltaTime   = 0.0f;  // seconds since last frame
    int    width       = 0;     // viewport width
    int    height      = 0;     // viewport height
    GLuint defaultShader = 0;   // host-compiled fallback shader
    GLuint vao         = 0;     // pre-allocated geometry VAO
    GLuint vbo         = 0;     // pre-allocated geometry VBO
};