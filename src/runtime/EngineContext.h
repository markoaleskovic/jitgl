#pragma once
#include <glad/gl.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

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
