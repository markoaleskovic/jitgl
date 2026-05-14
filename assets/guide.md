# Runtime State Guide
This explains why the default workspace code caches program handles, uniform locations, and shader hashes in `STATE_I()`.

---

### Why STATE_I Exists
* Your JIT code is recompiled/reloaded often, but `EngineContext` state arrays persist across reloads and workspace switches.
* `STATE_I(index)` and `STATE_F(index)` are stable per-workspace storage for values your code needs between frames.
* `ctx->state_buffer` is a stable 4096-byte POD buffer for transferable per-workspace state.
* `jit_alloc(...)` allocates from a host-managed arena that resets automatically on hot-reload.
* You can request a cold reset from code via `ctx->reset_state()` / `jit_request_hard_reset(ctx)`.
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
* Auto-injected ABI macro is `JIT_WORKSPACE_STATE_ABI_HASH`; call `jit_state_guard(ctx, JIT_WORKSPACE_STATE_ABI_HASH)` in `init()`.

> **Note:** Open this anytime from **Help -> Runtime State Guide**.
