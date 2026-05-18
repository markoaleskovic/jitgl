// Shared GLSL library available to all workspace shaders via:
//   #include "shared.glsl"

float jitgl_wave01(float t) {
    return 0.5 + 0.5 * sin(t);
}
