#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyePos;
    vec4 viewport;
    vec4 fogParams;
    vec4 fogColor;
} camera;

layout(push_constant) uniform BillboardPush {
    vec4 anchor;     // xyz = worldAnchor, w = unused
    vec4 right;      // xyz = billboard right, w = unused
    vec4 up;         // xyz = billboard up, w = unused
    vec2 halfSize;
    vec2 uvMin;
    vec2 uvMax;
    float strokeWidth;
    int shapeMode;
    vec4 color;
    vec4 stroke;
} push;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec2 outTexUV;

const vec2 kQuad[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

void main() {
    vec2 quad = kQuad[gl_VertexIndex % 6];
    vec3 worldPos = push.anchor.xyz
                  + push.right.xyz * (quad.x * push.halfSize.x)
                  + push.up.xyz    * (quad.y * push.halfSize.y);

    // Flip Y so UV origin is top-left (matches Skia/CSS/image pixel layout)
    vec2 uv = vec2(quad.x * 0.5 + 0.5, 0.5 - quad.y * 0.5);
    outUV = uv;
    outTexUV = mix(push.uvMin, push.uvMax, uv);
    gl_Position = camera.viewProj * vec4(worldPos, 1.0);
}
