# Inputs Guide

JITGL forwards keyboard and mouse state from the host window into your scene each frame. This page walks you through everything needed to drive a scene with input.

---

### 1. Enable input capture

Input is **off by default** so the editor never silently grabs keystrokes while you are typing in `scene.cpp`.

* Tick the **Capture Input** checkbox above the renderer view, or press **F1** to toggle it.
* Tick the **Recenter Cursor** button (or press **F2**) to put the host into mouse-look mode: the cursor is hidden and warped to the renderer center every frame so `mouseDX` / `mouseDY` keep delivering deltas without the cursor drifting off-screen. Disabling Capture Input also releases the cursor.
* Input only reaches your scene when **all three** of these are true:
    1. Capture is enabled in the UI.
    2. The renderer panel has focus (click the rendered image).
    3. ImGui is not consuming the input (so typing in the editor never moves things in the scene).
* A green border around the viewport means input is actively flowing to JIT code. An amber border means capture is enabled but a gate is currently blocking input (renderer not focused, ImGui has focus, etc.).
* If a compile **fails**, capture auto-disables so you do not keep driving a stale program.

---

### 2. Set up a workspace that reads input

A minimal input-aware scene needs:

1. A persistent shader program cached in `STATE_I(0)`.
2. Cached uniform locations (`STATE_I(1..N)`) so you do not re-query each frame.
3. An `update(ctx)` that mutates `STATE_F`/`STATE_I` from input.
4. A `renderFrame(ctx)` that uses the cached values.

Skeleton:

```cpp
extern "C" void init(EngineContext* ctx) {
    (void)jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH);
    if (STATE_I(0) == 0) {
        GLuint vs = jit_compile_shader(GL_VERTEX_SHADER,   JIT_WORKSPACE_VERTEX_SHADER);
        GLuint fs = jit_compile_shader(GL_FRAGMENT_SHADER, JIT_WORKSPACE_FRAGMENT_SHADER);
        STATE_I(0) = jit_link_program(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
        STATE_I(1) = glGetUniformLocation((GLuint)STATE_I(0), "uOffset");
    }
}

extern "C" void update(EngineContext* ctx) {
    const float speed = 1.5f * ctx->deltaTime;
    if (KEY_DOWN(ctx, JITGL_KEY_W)) STATE_F(1) += speed;
    if (KEY_DOWN(ctx, JITGL_KEY_S)) STATE_F(1) -= speed;
    if (KEY_DOWN(ctx, JITGL_KEY_A)) STATE_F(0) -= speed;
    if (KEY_DOWN(ctx, JITGL_KEY_D)) STATE_F(0) += speed;

    if (KEY_PRESSED(ctx, JITGL_KEY_SPACE)) {
        STATE_I(2) ^= 1u; // fires once per press
    }
}

extern "C" void renderFrame(EngineContext* ctx) {
    glUseProgram((GLuint)STATE_I(0));
    glUniform2f((GLint)STATE_I(1), STATE_F(0), STATE_F(1));
    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
```

A complete working version ships at `assets/input_example_scene.cpp` - paste it over a workspace's `scene.cpp` to try it.

---

### 3. Available helpers

Always use the macros - never read GLFW/OS key codes directly. They are stable across windowing-backend changes.

* `KEY_DOWN(ctx, k)` - key currently held this frame.
* `KEY_PRESSED(ctx, k)` - fires exactly once on the press transition.
* `KEY_RELEASED(ctx, k)` - fires exactly once on the release transition.
* `MOUSE_DOWN/PRESSED/RELEASED(ctx, b)` - same shape for mouse buttons.
* `MOUSE_IN_VIEWPORT(ctx)` - cursor is inside the renderer rect.
* `INPUTS_ENABLED(ctx)` - capture is enabled and a gate is not blocking input.

Keys: `JITGL_KEY_A..Z`, `JITGL_KEY_0..9`, `JITGL_KEY_SPACE`, `JITGL_KEY_ESCAPE`, `JITGL_KEY_ENTER`, `JITGL_KEY_TAB`, arrow keys, `JITGL_KEY_LSHIFT`, `JITGL_KEY_LCTRL`, `JITGL_KEY_F1..F12`. The full list is on the **engine.hpp** page (next tab).

Mouse buttons: `JITGL_MOUSE_LEFT`, `JITGL_MOUSE_RIGHT`, `JITGL_MOUSE_MIDDLE`.

---

### 4. Mouse position and scroll

* `ctx->input.mouseX / mouseY` - cursor in renderer-FBO pixel coords (0,0 = top-left).
* `ctx->input.mouseDX / mouseDY` - delta since last frame, in window pixels.
* `ctx->input.mouseNdcX / mouseNdcY` - cursor in NDC, in `[-1, 1]` across the panel. Use this for shader uniforms.
* `ctx->input.mouseScrollX / mouseScrollY` - scroll deltas this frame.

All values are zero when a gate is blocking input, so you never need to special-case "scene is paused".

---

### 5. Things to watch out for

* **Hot-reload preserves held state.** Edits do not produce a spurious `KEY_PRESSED` for a key that was already down at swap time, but `KEY_DOWN` will still report the key as held.
* **`KEY_PRESSED` / `KEY_RELEASED` fire once.** Do not gate persistent actions on them in `renderFrame` - handle them in `update` where state changes belong, otherwise the action runs zero or many times per visible frame.
* **Frame-lag on focus changes.** Mouse-position math uses the renderer rect captured during the previous UI frame. On the very first frame after focus changes, `mouseNdcX/Y` can lag by one frame.
* **Pipeline-chain mode shares state.** Every pass in the chain receives the *same* `InputState` snapshot for that frame - read it the same way in each pass.
* **Compile failure disables capture.** If your edit breaks compilation, JITGL drops capture so you do not keep driving a stale program. Re-tick the checkbox once compile succeeds.
* **ImGui wins over your scene.** If you are typing in the editor, `KEY_DOWN(...)` returns 0 for everything. This is intentional.
* **Render only - read in `update`.** Treat `renderFrame` as a pure consumer of `STATE_F`/`STATE_I`. Reading input there works, but mixing read-and-write across both hooks makes hot-reload behavior harder to reason about.
* **`mouseX/Y` is in FBO pixels, not viewport pixels.** If you resize the renderer panel and the FBO is recreated, the coordinate system follows the FBO.

---

> **Tip:** the bundled `assets/input_example_scene.cpp` drives a quad with WASD, tints it with Space, recenters with R, and lights up near the mouse - it's a fast way to sanity-check that capture is wired up correctly.
