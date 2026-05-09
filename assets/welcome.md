# JITGL
Live C++ + GLSL playground with per-workspace runtime state.

---

### How It Works
* Each workspace has two files: `scene.cpp` and `shader.glsl`.
* `scene.cpp` is JIT-compiled C++ and drives frame updates/rendering.
* `shader.glsl` contains your GPU shaders and is hot-swapped with `scene.cpp`.
* Edits auto-save and hot-reload while preserving workspace-specific state arrays.

---

### Shortcuts
| Shortcut | Action |
| --- | --- |
| **Ctrl+Tab** | Switch active editor file (`scene.cpp` <-> `shader.glsl`) |
| **Ctrl+`** | Cycle to next workspace |
| **Ctrl+N** | Open Create Workspace dialog |
| **Ctrl+T** | Toggle UI theme (dark/light) |
| **Ctrl+1..9** | Jump directly to workspace 1..9 |
| **Ctrl+0** | Jump directly to workspace 10 |
| **Ctrl++ / Ctrl+-** | Increase / decrease UI DPI scale |

---

### scene.cpp Compile Requirements
* Compiled as C++20 with engine/OpenGL helpers pre-injected.
* Define at least one entry point with `extern "C"`: `init`, `update`, `renderFrame`, `shutdown`.
* Use `EngineContext* ctx` signature for those entry points.
* Shader access macros are injected: `JIT_WORKSPACE_VERTEX_SHADER`, `JIT_WORKSPACE_FRAGMENT_SHADER`, `JIT_WORKSPACE_SHADER_HASH`.

### shader.glsl Compile Requirements
* Must include both sections: `#type vertex` and `#type fragment`.
* Each section must compile as GLSL (default templates use `#version 330 core`).
* Keep uniforms and usage aligned with `scene.cpp` logic.

> **Tip:** workspace labels are indexed (1:, 2:, ...), matching Ctrl+number switching.
> **Note:** Need deeper runtime details? Open **Help -> Runtime State Guide**.
