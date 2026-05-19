# Showcase Workspace

This startup workspace is designed to demonstrate the full JITGL workflow immediately:

- live code edits in `scene.cpp`
- live shader edits in `shader.glsl`
- auto-discovered uniform controls (float / vec2 / vec3 / vec4 / int / bool)
- grouped controls, ranges, steps, toggles, and color pickers

## What To Try

1. Open `shader.glsl` and change any math expression or color blend.
2. In **Uniform Controls** (top-right), tweak `uMirrorCount`, `uMode`, and `uWarp`.
3. Change palette colors (`uColorA`, `uColorB`, `uAccentColor`) to verify color pickers.
4. Toggle `uInvert` and `uVignette` to test bool-driven branches.
5. Drag a PNG / JPG / OBJ from your file manager onto the JITGL window. It lands in this workspace's `assets/` folder and shows up in the **Assets** tab. Right-click the row and "Copy load call" to paste a ready-to-use `jit_load_texture` / `jit_load_mesh` into `scene.cpp`.

## Importing assets

* The **Assets** tab (next to Renderer / Pipeline / Console) lists everything under `<workspace>/assets/` and the shared root `assets/`.
* Drop OS files onto the JITGL window to copy them into the current workspace's `assets/`. The Assets tab status line confirms the import.
* Each row's context menu gives you **Copy load call**, **Copy path**, **Reveal in file manager**, and **Delete** (workspace assets only).
* Image edits hot-reload directly: save the PNG in your image editor and the texture re-uploads into the same GL id without a C++ recompile. `.obj` edits do the same on the VAO/VBO/EBO.
* File-naming conventions for sRGB are documented on the **Runtime State** guide page. tl;dr: `_normal`, `_rough`, `_metal`, `_data`, `_linear`, etc. opt out of sRGB; everything else loads as `GL_SRGB8_ALPHA8` by default.
* `.jws` workspace exports and LAN-shared workspaces include `assets/`, so a peer receiving your scene gets the textures and meshes alongside the code.

## Uniform Annotation Hints

Use comments directly on uniform declarations:

```glsl
uniform float uAmount; // @range(0.0, 2.0) @step(0.01)
uniform vec3 uTint;    // @color
uniform bool uEnabled; // @toggle
uniform vec2 uOffset;  // @group("Camera")
uniform float uDebug;  // @hidden
```

## Startup Behavior

The checkbox at the bottom of this guide controls whether this showcase and guide open automatically on startup.

You can re-enable them anytime from:

`Help -> Enable Showcase Startup`. Reopen this page anytime from `Help -> Guides`.
