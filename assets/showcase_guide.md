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

`Help -> Enable Showcase Startup`
