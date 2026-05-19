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

---

### Loading textures from disk

`jit_load_texture(ctx, "wood.png")` returns a `JitTexture` whose `.id` is a real GL texture handle. The host's asset registry:

* Looks under the workspace's own `assets/` directory first, then the shared root `assets/` directory.
* Decodes via stb_image (PNG/JPG/BMP/TGA/PSD/HDR/GIF), generates a mipmap chain, and applies 16x anisotropy when supported.
* Defaults color textures to `GL_SRGB8_ALPHA8`. Filenames containing `_normal`, `_nrm`, `_data`, `_linear`, `_rough`, `_metal`, `_ao`, `_height`, or `_depth` upload as linear `GL_RGBA8` instead. `.hdr` files always upload as linear `GL_RGBA16F`.
* Returns a permanent magenta/black checker handle if the path does not resolve or the decode fails -- your draw call still binds something and the breakage is visible on screen.

```cpp
extern "C" void init(EngineContext* ctx) {
    JitTexture wood = jit_load_texture(ctx, "wood.png");
    STATE_I(0) = wood.id;
    STATE_I(1) = wood.width;
    STATE_I(2) = wood.height;
}
```

Calling `jit_load_texture` is idempotent: it returns the cached handle for any path already loaded, so calling it every frame is cheap. Editing the file on disk hot-reloads into the same GL id -- no C++ recompile required.

---

### Loading meshes from disk

`jit_load_mesh(ctx, "head.obj")` returns a `JitMesh` whose `.vao` is a real VAO with the canonical interleaved layout:

```
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aTangent;   // w stores bitangent sign
```

Format support: Wavefront `.obj` (triangulated; multi-material files are flattened). Missing normals are synthesised from face geometry (area-weighted, smooth at shared positions). Missing tangents are computed per-triangle from UVs and averaged at shared vertices. Vertex de-dup is keyed on the source `(pos, uv, normal)` triplet so the produced index buffer is clean.

Caps: 10M vertices, 30M indices per mesh. Beyond that the load fails with a clear error and you get the fallback handle.

Failure mode: `is_fallback == 1` and the handle points at a permanent unit cube. Your draw call still produces geometry -- you immediately see what is broken on screen.

```cpp
extern "C" void init(EngineContext* ctx) {
    JitMesh head = jit_load_mesh(ctx, "head.obj");
    STATE_I(0) = head.vao;
    STATE_I(1) = head.index_count;
}

extern "C" void renderFrame(EngineContext* ctx) {
    glBindVertexArray((GLuint)STATE_I(0));
    glDrawElements(GL_TRIANGLES, (GLsizei)STATE_I(1), GL_UNSIGNED_INT, nullptr);
}
```

Editing the `.obj` on disk hot-reloads into the same VAO/VBO/EBO. `bbox_min` / `bbox_max` are populated and useful for framing a camera at runtime.

---

### Workspace packaging

Each workspace has its own `assets/` directory at `workspace/<name>/assets/`. New workspaces get one created automatically. The `.jws` export bundles every regular file under `assets/` (cap: 256 files, 50 MB each) into the package; importing a `.jws` materialises them back into the destination workspace's `assets/`. LAN share rides the same package format, so peers receive textures and meshes alongside the scene.

> **Note:** Open this guide anytime from **Help -> Guides** and switch to **Runtime State**. For input details, switch to the **Inputs** page.
