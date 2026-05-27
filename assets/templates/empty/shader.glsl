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

uniform vec3 uColor; // @color @group("Look")

void main() {
    FragColor = vec4(uColor, 1.0);
}
