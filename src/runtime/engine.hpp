// engine.hpp -- auto-prepended by the JIT engine. Do not include manually.
#ifndef ENGINE_HPP_JIT_PREAMBLE
#define ENGINE_HPP_JIT_PREAMBLE

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <glad/gl.h>
#include "runtime/EngineContext.h"

// Helper macros for easy access to persistent state
#define STATE_I(idx) (ctx->state_i[idx])
#define STATE_F(idx) (ctx->state_f[idx])

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
