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

#endif // ENGINE_HPP_JIT_PREAMBLE
