#type vertex
#version 330 core
layout (location = 0) in vec2 aPos;
out vec2 vUv;

void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}

#type fragment
#version 330 core
in vec2 vUv;
out vec4 FragColor;

uniform float uTime; // @hidden
uniform vec2 uResolution; // @hidden

uniform float uZoom; // @range(0.4, 4.0) @step(0.01) @group("Camera")
uniform vec2 uPan; // @range(-2.0, 2.0) @step(0.01) @group("Camera")

uniform int uMirrorCount; // @range(1.0, 14.0) @step(1.0) @group("Pattern")
uniform int uMode; // @range(0.0, 3.0) @step(1.0) @group("Pattern")
uniform float uPulse; // @range(0.0, 2.8) @step(0.01) @group("Pattern")
uniform vec2 uWarp; // @range(0.0, 4.0) @step(0.01) @group("Pattern")

uniform vec3 uColorA; // @color @group("Palette")
uniform vec3 uColorB; // @color @group("Palette")
uniform vec4 uAccentColor; // @color @group("Palette")

uniform bool uInvert; // @toggle @group("FX")
uniform bool uVignette; // @toggle @group("FX")
uniform float uGlow; // @range(0.0, 2.2) @step(0.01) @group("FX")
uniform float uGrain; // @range(0.0, 0.2) @step(0.001) @group("FX")

const float TAU = 6.28318530718;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 uv = vUv * 2.0 - 1.0;
    uv.x *= uResolution.x / max(uResolution.y, 1.0);
    uv = (uv + uPan) / max(uZoom, 0.0001);

    float angle = atan(uv.y, uv.x);
    float radius = length(uv);

    int mirrors = max(uMirrorCount, 1);
    float sector = TAU / float(mirrors);
    angle = abs(mod(angle + 0.5 * sector, sector) - 0.5 * sector);

    vec2 p = vec2(cos(angle), sin(angle)) * radius;
    float t = uTime * (0.35 + uPulse);
    p += vec2(
        sin((p.y + t) * (1.5 + uWarp.x)),
        cos((p.x - t * 0.7) * (1.5 + uWarp.y))
    ) * 0.25;

    float base;
    if (uMode == 0) {
        base = sin(p.x * 7.0 + t) + cos(p.y * 6.0 - t * 0.7);
    } else if (uMode == 1) {
        base = sin(length(p) * 13.0 - t * 2.5) + atan(p.y, p.x) * 3.0;
    } else if (uMode == 2) {
        base = sin((p.x + p.y) * 9.0 + t * 1.4) * cos((p.x - p.y) * 9.0 - t * 1.1);
    } else {
        base = cos(p.x * 10.0 + t * 1.9) + sin(p.y * 11.0 - t * 1.6);
    }

    float waves = sin(base * 1.35 + radius * 8.0 - t * 2.2);
    float glowMask = exp(-radius * (1.2 + uGlow));
    float colorMix = smoothstep(-0.25, 0.35, waves);

    vec3 color = mix(uColorA, uColorB, colorMix);
    color += uAccentColor.rgb * uAccentColor.a * pow(abs(waves), 2.2) * glowMask * (0.7 + uGlow);

    if (uVignette) {
        float vignette = 1.0 - smoothstep(0.35, 1.35, radius);
        color *= vignette;
    }

    if (uGrain > 0.0) {
        float noise = hash12(gl_FragCoord.xy + uTime * 60.0) - 0.5;
        color += noise * uGrain;
    }

    if (uInvert) {
        color = 1.0 - color;
    }

    FragColor = vec4(color, 1.0);
}
