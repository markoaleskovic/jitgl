// engine.hpp -- auto-prepended by the JIT engine. Do not include manually.
#ifndef ENGINE_HPP_JIT_PREAMBLE
#define ENGINE_HPP_JIT_PREAMBLE

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <glad/gl.h>
#include "runtime/EngineContext.h"

// Helper macros for easy access to persistent state
#define STATE_I(idx) (ctx->state_i[idx])
#define STATE_F(idx) (ctx->state_f[idx])

// ---- Input ---------------------------------------------------------------
// These codes currently match GLFW key codes, but JIT scenes should always
// reference the JITGL_KEY_* constants so they keep working if the host
// windowing layer changes. KEY_DOWN reports the current held state, while
// KEY_PRESSED / KEY_RELEASED fire for exactly one frame on the transition.
// The input snapshot is zeroed when input capture is off (toggle in the UI),
// when the renderer panel does not have focus, or when ImGui is consuming
// input -- so JIT code can read inputs unconditionally without polluting
// editor typing.
#define JITGL_KEY_SPACE   32
#define JITGL_KEY_APOSTROPHE 39
#define JITGL_KEY_COMMA   44
#define JITGL_KEY_MINUS   45
#define JITGL_KEY_PERIOD  46
#define JITGL_KEY_SLASH   47
#define JITGL_KEY_0       48
#define JITGL_KEY_1       49
#define JITGL_KEY_2       50
#define JITGL_KEY_3       51
#define JITGL_KEY_4       52
#define JITGL_KEY_5       53
#define JITGL_KEY_6       54
#define JITGL_KEY_7       55
#define JITGL_KEY_8       56
#define JITGL_KEY_9       57
#define JITGL_KEY_A       65
#define JITGL_KEY_B       66
#define JITGL_KEY_C       67
#define JITGL_KEY_D       68
#define JITGL_KEY_E       69
#define JITGL_KEY_F       70
#define JITGL_KEY_G       71
#define JITGL_KEY_H       72
#define JITGL_KEY_I       73
#define JITGL_KEY_J       74
#define JITGL_KEY_K       75
#define JITGL_KEY_L       76
#define JITGL_KEY_M       77
#define JITGL_KEY_N       78
#define JITGL_KEY_O       79
#define JITGL_KEY_P       80
#define JITGL_KEY_Q       81
#define JITGL_KEY_R       82
#define JITGL_KEY_S       83
#define JITGL_KEY_T       84
#define JITGL_KEY_U       85
#define JITGL_KEY_V       86
#define JITGL_KEY_W       87
#define JITGL_KEY_X       88
#define JITGL_KEY_Y       89
#define JITGL_KEY_Z       90
#define JITGL_KEY_ESCAPE  256
#define JITGL_KEY_ENTER   257
#define JITGL_KEY_TAB     258
#define JITGL_KEY_BACKSPACE 259
#define JITGL_KEY_RIGHT   262
#define JITGL_KEY_LEFT    263
#define JITGL_KEY_DOWN    264
#define JITGL_KEY_UP      265
#define JITGL_KEY_F1      290
#define JITGL_KEY_F2      291
#define JITGL_KEY_F3      292
#define JITGL_KEY_F4      293
#define JITGL_KEY_F5      294
#define JITGL_KEY_F6      295
#define JITGL_KEY_F7      296
#define JITGL_KEY_F8      297
#define JITGL_KEY_F9      298
#define JITGL_KEY_F10     299
#define JITGL_KEY_F11     300
#define JITGL_KEY_F12     301
#define JITGL_KEY_LSHIFT  340
#define JITGL_KEY_LCTRL   341
#define JITGL_KEY_LALT    342
#define JITGL_KEY_RSHIFT  344
#define JITGL_KEY_RCTRL   345
#define JITGL_KEY_RALT    346

#define JITGL_MOUSE_LEFT   0
#define JITGL_MOUSE_RIGHT  1
#define JITGL_MOUSE_MIDDLE 2

#define KEY_DOWN(ctx, k)     ((ctx)->input.keyDown[(k)] != 0)
#define KEY_PRESSED(ctx, k)  ((ctx)->input.keyPressed[(k)] != 0)
#define KEY_RELEASED(ctx, k) ((ctx)->input.keyReleased[(k)] != 0)
#define MOUSE_DOWN(ctx, b)     ((ctx)->input.mouseDown[(b)] != 0)
#define MOUSE_PRESSED(ctx, b)  ((ctx)->input.mousePressed[(b)] != 0)
#define MOUSE_RELEASED(ctx, b) ((ctx)->input.mouseReleased[(b)] != 0)
#define INPUTS_ENABLED(ctx)    ((ctx)->input.inputsEnabled != 0)
#define MOUSE_IN_VIEWPORT(ctx) ((ctx)->input.mouseInViewport != 0)

extern "C" void* jit_heap_alloc(std::size_t size);
extern "C" void* jit_heap_alloc_aligned(std::size_t size, std::size_t alignment);
extern "C" void jit_heap_free(void* ptr) noexcept;

inline constexpr std::uint64_t jit_fnv1a64(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    return hash;
}

inline void* jit_alloc_ctx(EngineContext* context, std::size_t size) {
    return context ? context->allocate(size) : nullptr;
}

inline void* jit_alloc_aligned_ctx(EngineContext* context, std::size_t size, std::size_t alignment) {
    return context ? context->allocate(size, alignment) : nullptr;
}

template <typename T>
inline T* jit_alloc_type(EngineContext* context, std::size_t count = 1) {
    static_assert(!std::is_void_v<T>, "jit_alloc_type<void> is not allowed");
    const std::size_t total = sizeof(T) * count;
    return static_cast<T*>(jit_alloc_aligned_ctx(context, total, alignof(T)));
}

template <typename T>
inline T* jit_state_buffer(EngineContext* context) {
    if (!context || sizeof(T) > context->state_buffer.size()) {
        return nullptr;
    }
    return reinterpret_cast<T*>(context->state_buffer.data());
}

inline bool jit_state_guard(EngineContext* context, std::uint64_t expectedHash) {
    if (!context || expectedHash == 0) {
        return true;
    }
    if (context->state_abi_hash == expectedHash) {
        return true;
    }
    context->clear_runtime_state();
    context->state_abi_hash = expectedHash;
    return false;
}

inline void jit_request_hard_reset(EngineContext* context) {
    if (context) {
        context->request_reset_state();
    }
}

// ---- Asset loading ------------------------------------------------------
// Loads (or returns the cached handle for) a texture under the active
// workspace's `assets/` directory or the root-shared `assets/` directory.
// Always returns a valid GL texture id: on failure the handle points at a
// 2x2 magenta/black checkerboard so shaders sample it normally and the user
// sees the breakage on screen instead of a silent black draw. Calling this
// every frame is cheap (one hash lookup); the conventional spot is init().
inline JitTexture jit_load_texture(EngineContext* context, const char* path) {
    if (!context || !context->load_texture_fn || !path) {
        return JitTexture{};
    }
    return context->load_texture_fn(context, path);
}

// Loads (or returns the cached handle for) a mesh under the active workspace's
// `assets/` or the root-shared `assets/`. Supported formats: Wavefront `.obj`
// (single-material flattened). The host returns a VAO whose attribute layout
// is fixed:
//   layout(location = 0) in vec3 aPos;
//   layout(location = 1) in vec3 aNormal;
//   layout(location = 2) in vec2 aUv;
//   layout(location = 3) in vec4 aTangent;   // w stores bitangent sign
// Always returns a valid handle; on failure it points at a permanent unit
// cube with `is_fallback = 1` so your draw call still produces visible
// geometry while you fix the path.
inline JitMesh jit_load_mesh(EngineContext* context, const char* path) {
    if (!context || !context->load_mesh_fn || !path) {
        return JitMesh{};
    }
    return context->load_mesh_fn(context, path);
}

// Convenience macros for the common `EngineContext* ctx` callback signature.
#define jit_alloc(size) jit_alloc_ctx((ctx), (size))
#define jit_alloc_aligned(size, alignment) jit_alloc_aligned_ctx((ctx), (size), (alignment))

// Shader helper (internally provided by host or written here)
// For now, let's keep it simple and let the user use standard GL calls,
// but we can provide a small helper for checking errors.
inline GLuint jit_compile_shader(GLenum type, const char* source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint success;
    glGetShaderiv(s, GL_COMPILE_STATUS, &success);
    if (!success) {
        std::array<char, 512> info{};
        glGetShaderInfoLog(s, static_cast<GLsizei>(info.size()), nullptr, info.data());
        std::printf("[JIT Shader Error] %s\n", info.data());
    }
    return s;
}

inline GLuint jit_link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint success;
    glGetProgramiv(p, GL_LINK_STATUS, &success);
    if (!success) {
        std::array<char, 512> info{};
        glGetProgramInfoLog(p, static_cast<GLsizei>(info.size()), nullptr, info.data());
        std::printf("[JIT Program Link Error] %s\n", info.data());
    }
    return p;
}

inline GLuint jit_link_compute_program(GLuint cs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, cs);
    glLinkProgram(p);
    GLint success;
    glGetProgramiv(p, GL_LINK_STATUS, &success);
    if (!success) {
        std::array<char, 512> info{};
        glGetProgramInfoLog(p, static_cast<GLsizei>(info.size()), nullptr, info.data());
        std::printf("[JIT Compute Link Error] %s\n", info.data());
    }
    return p;
}

#endif // ENGINE_HPP_JIT_PREAMBLE
