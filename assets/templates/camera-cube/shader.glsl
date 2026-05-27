#type vertex
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 uViewProj;
uniform mat4 uModel;

out vec3 vWorldNormal;
out vec3 vWorldPos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    // Cube uses uniform scale + rotation; transpose(inverse(mat3(uModel)))
    // would be safer in general but this is just rotation, so it's fine.
    vWorldNormal = normalize(mat3(uModel) * aNormal);
    gl_Position = uViewProj * worldPos;
}

#type fragment
#version 330 core
in vec3 vWorldNormal;
in vec3 vWorldPos;
out vec4 FragColor;

uniform vec3 uLightDir;     // @group("Lighting")
uniform vec3 uBaseColor;    // @color @group("Surface")
uniform float uAmbient;     // @range(0.0, 1.0) @step(0.01) @group("Lighting")

void main() {
    vec3 n = normalize(vWorldNormal);
    vec3 l = normalize(uLightDir);
    float lambert = max(dot(n, l), 0.0);
    vec3 color = uBaseColor * (uAmbient + (1.0 - uAmbient) * lambert);

    // Hint of facet color from the normal so each face is distinguishable
    // even when lighting flattens them.
    color += 0.08 * (n * 0.5 + 0.5);

    FragColor = vec4(color, 1.0);
}
