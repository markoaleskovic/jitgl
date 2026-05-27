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

uniform float uTime;        // @hidden
uniform vec2 uResolution;   // @hidden

uniform float uSpeed;       // @range(0.0, 4.0) @step(0.01) @group("Motion")
uniform float uContrast;    // @range(0.2, 3.0) @step(0.01) @group("Look")
uniform vec3 uColorA;       // @color @group("Look")
uniform vec3 uColorB;       // @color @group("Look")
uniform vec3 uColorC;       // @color @group("Look")

// Smooth 3-stop gradient with a time-driven phase. The phase blends through
// the three colors in sequence, and pow(...) reshapes it to give a more
// pleasing curve than linear interpolation alone.
void main() {
    vec2 uv = vUv;
    float phase = sin(uTime * uSpeed * 0.5 + uv.x * 3.14159) * 0.5 + 0.5;
    phase = pow(phase, uContrast);

    vec3 a = mix(uColorA, uColorB, smoothstep(0.0, 0.5, phase));
    vec3 b = mix(uColorB, uColorC, smoothstep(0.5, 1.0, phase));
    vec3 color = mix(a, b, smoothstep(0.4, 0.6, phase));

    // Vertical fade for a subtle dimensional feel.
    color *= mix(0.85, 1.15, uv.y);

    FragColor = vec4(color, 1.0);
}
