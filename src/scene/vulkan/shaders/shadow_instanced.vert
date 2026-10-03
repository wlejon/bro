#version 450

layout(location = 0) in vec3 inPos;

// Per-instance attributes (rate = instance)
layout(location = 8)  in vec4 inInstRow0;
layout(location = 9)  in vec4 inInstRow1;
layout(location = 10) in vec4 inInstRow2;
layout(location = 11) in vec4 inInstColor;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightMVP;
} push;

void main() {
    mat4 instModel = mat4(
        vec4(inInstRow0.x, inInstRow1.x, inInstRow2.x, 0.0),
        vec4(inInstRow0.y, inInstRow1.y, inInstRow2.y, 0.0),
        vec4(inInstRow0.z, inInstRow1.z, inInstRow2.z, 0.0),
        vec4(inInstRow0.w, inInstRow1.w, inInstRow2.w, 1.0)
    );

    vec4 localPos = instModel * vec4(inPos, 1.0);
    gl_Position = push.lightMVP * localPos;
}
