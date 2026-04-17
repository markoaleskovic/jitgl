// engine.hpp -- auto-prepended by the JIT engine. Do not include manually.
#ifndef ENGINE_HPP_JIT_PREAMBLE
#define ENGINE_HPP_JIT_PREAMBLE

#include <glad/gl.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

struct EngineContext {
    float  time;
    float  deltaTime;
    int    width;
    int    height;
    unsigned int defaultShader;
    unsigned int vao;
    unsigned int vbo;
};

#endif // ENGINE_HPP_JIT_PREAMBLE