# Runtime State Guide
This explains why the default workspace code caches program handles, uniform locations, and shader hashes in `STATE_I()`.

---

### Why STATE_I Exists
* Your JIT code is recompiled/reloaded often, but `EngineContext` state arrays persist across reloads and workspace switches.
* `STATE_I(index)` and `STATE_F(index)` are stable per-workspace storage for values your code needs between frames.
* `ctx->state_buffer` is a stable 4096-byte POD buffer for transferable per-workspace state.
* `jit_alloc(...)` allocates from a host-managed arena that resets automatically on hot-reload.
* You can request a cold reset from code via `ctx->reset_state()` / `jit_request_hard_reset(ctx)`.
* `#include "shared.glsl"` is supported recursively, and include changes trigger recompiles in dependent workspaces.
* Without this storage, expensive setup work (shader compile/link, uniform lookups) would repeat unnecessarily.

---

### What These Lines Do
```cpp
STATE_I(0) = (uint32_t)program;
STATE_I(1) = (uint32_t)glGetUniformLocation(program, "uTime");
STATE_I(3) = JIT_WORKSPACE_SHADER_HASH;
```
* `STATE_I(0)`: stores the linked OpenGL program handle for reuse.
* `STATE_I(1)`: stores the `uTime` uniform location so you do not query it every frame.
* `STATE_I(3)`: stores the shader hash that was used to build the cached program.

---

### Why init() Reads These Values
```cpp
GLuint currentProgram = (GLuint)STATE_I(0);
const uint32_t cachedShaderHash = STATE_I(3);
```
* `currentProgram` tells you whether a valid program already exists.
* `cachedShaderHash` tells you whether `shader.glsl` changed since the cached program was built.
* If hash changed (or no program exists), you rebuild once in `init()`; otherwise you reuse the existing program.

---

### Why This Pattern Is Needed
* JIT compilation is frequent; GPU program creation is relatively expensive.
* Hash-based caching avoids unnecessary recompiles while still hot-reloading correctly when shader source changes.
* Persistent per-workspace state gives smooth iteration and keeps runtime behavior deterministic.

---

### Other Useful Details
* Export entry points with `extern "C"` so symbol lookup can find `init`/`update`/`renderFrame`/`shutdown`.
* Use a documented index map in your `scene.cpp` comments to avoid collisions (e.g. `0=program`, `1=uTime`, `3=hash`).
* Use `STATE_F` for persistent float values (timers, parameters, interpolation state).
* If `glGetUniformLocation` returns -1, the uniform may be optimized out or misnamed.
* Auto-injected shader macros are `JIT_WORKSPACE_VERTEX_SHADER`, `JIT_WORKSPACE_FRAGMENT_SHADER`, `JIT_WORKSPACE_SHADER_HASH`.
* Compute macros/hooks are also available: `JIT_WORKSPACE_COMPUTE_SHADER`, `JIT_WORKSPACE_HAS_COMPUTE`, and `dispatchCompute(ctx)`.
* Auto-injected ABI macro is `JIT_WORKSPACE_STATE_ABI_HASH`; call `jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH)` in `init()`.
* Pipeline resources are auto-bound by sampler name; `<outputName>_prev` samples previous-frame output for temporal effects.

> **Note:** Open this anytime from **Help -> Runtime State Guide**.

---

# Input Guide

JITGL can forward keyboard and mouse input from the host window into your scene's `update()`, `dispatchCompute()`, and `renderFrame()` functions. Input is **off by default**: the editor never silently captures keystrokes while you are coding.

### Enabling input

* Check the **Capture Input** box above the renderer view, or press **F1** to toggle.
* Input only reaches your scene when **all three** of these are true:
    1. Capture is enabled in the UI.
    2. The renderer panel has focus (click the rendered image).
    3. ImGui is not consuming the input (so typing in the editor never moves things in the scene).
* The viewport border turns **green** when input is actively flowing to JIT code, and **amber** when capture is enabled but a gate is blocking input. The badge next to the checkbox reflects the same state.
* If a compile fails, capture is **auto-disabled** so you do not drive a stale program.

### Reading input from JIT code

Input state lives in `ctx->input`. Read it through the helper macros so your scene survives any future windowing-backend change:

```cpp
extern "C" void update(EngineContext* ctx) {
    const float speed = 1.5f * ctx->deltaTime;
    if (KEY_DOWN(ctx, JITGL_KEY_W)) STATE_F(0) += speed;
    if (KEY_DOWN(ctx, JITGL_KEY_S)) STATE_F(0) -= speed;
    if (KEY_DOWN(ctx, JITGL_KEY_A)) STATE_F(1) -= speed;
    if (KEY_DOWN(ctx, JITGL_KEY_D)) STATE_F(1) += speed;
    if (KEY_PRESSED(ctx, JITGL_KEY_SPACE)) {
        // fires once on the frame the spacebar transitions to down
        STATE_I(0) ^= 1u;
    }
    if (MOUSE_DOWN(ctx, JITGL_MOUSE_LEFT) && MOUSE_IN_VIEWPORT(ctx)) {
        // ctx->input.mouseNdcX / mouseNdcY are in [-1, 1] across the panel.
        STATE_F(2) = ctx->input.mouseNdcX;
        STATE_F(3) = ctx->input.mouseNdcY;
    }
}
```

### Available constants and helpers

* Keys: `JITGL_KEY_A`..`JITGL_KEY_Z`, `JITGL_KEY_0`..`JITGL_KEY_9`, `JITGL_KEY_SPACE`, `JITGL_KEY_ESCAPE`, `JITGL_KEY_ENTER`, `JITGL_KEY_TAB`, `JITGL_KEY_LEFT/RIGHT/UP/DOWN`, `JITGL_KEY_LSHIFT`, `JITGL_KEY_LCTRL`, `JITGL_KEY_F1`..`JITGL_KEY_F12`, and more (see `runtime/engine.hpp`).
* Mouse buttons: `JITGL_MOUSE_LEFT`, `JITGL_MOUSE_RIGHT`, `JITGL_MOUSE_MIDDLE`.
* Helpers:
    * `KEY_DOWN(ctx, k)` – currently held.
    * `KEY_PRESSED(ctx, k)` – fired only on the frame the key was pressed.
    * `KEY_RELEASED(ctx, k)` – fired only on the frame the key was released.
    * `MOUSE_DOWN/PRESSED/RELEASED(ctx, b)` – same for mouse buttons.
    * `INPUTS_ENABLED(ctx)`, `MOUSE_IN_VIEWPORT(ctx)` – status flags.

### Position and scroll

* `ctx->input.mouseX / mouseY` – cursor in renderer-FBO pixel coords (0,0 = top-left).
* `ctx->input.mouseDX / mouseDY` – delta since last frame, in window pixels.
* `ctx->input.mouseNdcX / mouseNdcY` – cursor in NDC; useful for shader uniforms.
* `ctx->input.mouseScrollX / mouseScrollY` – scroll deltas this frame.
* All values are zero when a gate is blocking input.

### Gotchas

* Hot-reload preserves "is this key held?" but never produces a spurious `KEY_PRESSED` for a key that was already down at swap time.
* In pipeline-chain mode, every pass receives the **same** `InputState` snapshot.
* The mouse-position math uses the renderer panel rect captured during the previous UI frame, so on the very first frame after focus changes it can lag by one frame.

> **Note:** A bundled WASD example lives in `assets/input_example_scene.cpp`. Open it via **File -> Import Workspace** or copy-paste into a new workspace's `scene.cpp`.
