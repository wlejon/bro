#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in float inSize;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inRot;
layout(location = 4) in float inFrame;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyeWorld;
    vec4 viewport;
    vec4 fogParams;
    vec4 fogColor;
} camera;

layout(push_constant) uniform ParticlePush {
    mat4 model;
    vec4 camRight;
    vec4 camUp;
    vec4 params; // x: cols, y: rows, z: mode, w: softDist
} push;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

const vec2 kQuad[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

void main() {
    vec2 aQuad = kQuad[gl_VertexIndex % 6];
    float c = cos(inRot);
    float s = sin(inRot);
    vec2 corner = vec2(aQuad.x * c - aQuad.y * s,
                       aQuad.x * s + aQuad.y * c);
    vec3 world = (push.model * vec4(inPos, 1.0)).xyz;
    float halfSize = inSize * 0.5;
    vec3 worldPos = world
                  + push.camRight.xyz * (corner.x * halfSize)
                  + push.camUp.xyz    * (corner.y * halfSize);

    // UV origin top-left (matches image pixel layout)
    vec2 uv = vec2(aQuad.x * 0.5 + 0.5, 0.5 - aQuad.y * 0.5);
    float cols = push.params.x;
    float rows = push.params.y;
    if (cols > 1.0 || rows > 1.0) {
        int icols = int(cols);
        int irows = int(rows);
        int frame = clamp(int(inFrame), 0, icols * irows - 1);
        int cx = frame - (frame / icols) * icols;
        int cy = frame / icols;
        uv = (vec2(float(cx), float(cy)) + uv) / vec2(cols, rows);
    }

    outUV = uv;
    outColor = inColor;
    gl_Position = camera.viewProj * vec4(worldPos, 1.0);
}
