#pragma once
#include <glad/gl.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// JIT-visible texture handle. Filled by jit_load_texture / the host
// AssetRegistry. `id` is a real GL texture id usable with glBindTexture etc.;
// `is_srgb` / `is_hdr` describe the upload format so shaders can branch.
struct JitTexture {
    unsigned int id = 0;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::uint8_t is_hdr = 0;
    std::uint8_t is_srgb = 0;
    std::uint8_t is_fallback = 0;
    std::uint8_t _pad = 0;
};

// JIT-visible mesh handle. The host owns the underlying VAO/VBO/EBO and
// guarantees the canonical interleaved layout described in engine.hpp:
//   loc 0  vec3 position
//   loc 1  vec3 normal
//   loc 2  vec2 uv
//   loc 3  vec4 tangent (w = bitangent sign)
struct JitMesh {
    unsigned int vao = 0;
    unsigned int vbo = 0;
    unsigned int ebo = 0;
    int index_count = 0;
    int vertex_count = 0;
    float bbox_min[3] = {0.0f, 0.0f, 0.0f};
    float bbox_max[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t is_fallback = 0;
    std::uint8_t _pad[3] = {0, 0, 0};
};

struct EngineContext;
extern "C" {
    using JitLoadTextureFn = JitTexture (*)(EngineContext* ctx, const char* path);
    using JitLoadMeshFn    = JitMesh    (*)(EngineContext* ctx, const char* path);
}

// Per-frame snapshot of keyboard and mouse state, populated by the host before
// each JIT callback. JIT code reads this through KEY_DOWN / MOUSE_DOWN helpers
// in engine.hpp; it should never write to it. When input capture is disabled
// the host zeroes this whole struct so JIT code sees a quiet world.
struct InputState {
    static constexpr std::size_t kKeyCount = 512;
    static constexpr std::size_t kMouseButtonCount = 8;

    std::array<std::uint8_t, kKeyCount> keyDown{};
    std::array<std::uint8_t, kKeyCount> keyPressed{};
    std::array<std::uint8_t, kKeyCount> keyReleased{};
    std::array<std::uint8_t, kMouseButtonCount> mouseDown{};
    std::array<std::uint8_t, kMouseButtonCount> mousePressed{};
    std::array<std::uint8_t, kMouseButtonCount> mouseReleased{};
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float mouseDX = 0.0f;
    float mouseDY = 0.0f;
    float mouseScrollX = 0.0f;
    float mouseScrollY = 0.0f;
    float mouseNdcX = 0.0f;
    float mouseNdcY = 0.0f;
    std::uint8_t inputsEnabled = 0;
    std::uint8_t mouseInViewport = 0;
    std::uint8_t _pad[6]{};
};

// Plain data, owned by Engine, passed into every JIT callback.
// Never delete or store this pointer -- the host manages its lifetime.
struct EngineContext {
    static constexpr std::size_t kStateBufferSize = 4096;

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
    std::array<uint32_t, 64> state_i{}; // Increased to 64
    std::array<float, 64> state_f{};    // Increased to 64
    std::array<unsigned char, kStateBufferSize> state_buffer{};
    void*    userData    = nullptr;
    uint64_t state_abi_hash = 0;

    // Per-run scratch arena (host-provided backing store).
    void* arena_base = nullptr;
    std::size_t arena_size = 0;
    std::size_t arena_offset = 0;

    // Request a host-side hard reset (consumed on the next frame).
    bool reset_state_requested = false;

    // Per-frame keyboard/mouse snapshot. Populated by Engine::UpdateInputState
    // before every update()/render()/compute() invocation. Zeroed when the
    // user has disabled input capture (or when ImGui is consuming input).
    InputState input{};

    // Opaque pointer to the host's per-workspace AssetRegistry. JIT scenes
    // never dereference this directly; the load_*_fn shims do. Host sets
    // `assets` whenever the active workspace changes; the function pointers
    // are set once during engine init.
    void* assets = nullptr;
    JitLoadTextureFn load_texture_fn = nullptr;
    JitLoadMeshFn    load_mesh_fn = nullptr;

    // Helper to get memory that lives only for the current run/reload.
    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
        if (size == 0 || arena_base == nullptr || arena_size == 0) {
            return nullptr;
        }
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return nullptr;
        }

        const std::size_t alignmentMask = alignment - 1;
        const std::size_t alignedOffset = (arena_offset + alignmentMask) & ~alignmentMask;
        if (alignedOffset > arena_size || size > (arena_size - alignedOffset)) {
            return nullptr;
        }

        void* ptr = static_cast<unsigned char*>(arena_base) + alignedOffset;
        arena_offset = alignedOffset + size;
        return ptr;
    }

    void reset_arena() {
        arena_offset = 0;
    }

    void request_reset_state() {
        reset_state_requested = true;
    }

    void reset_state() {
        request_reset_state();
    }

    bool consume_reset_state_request() {
        const bool requested = reset_state_requested;
        reset_state_requested = false;
        return requested;
    }

    void clear_runtime_state() {
        state_i.fill(0);
        state_f.fill(0.0f);
        state_buffer.fill(0);
        userData = nullptr;
        state_abi_hash = 0;
        reset_arena();
    }
};
