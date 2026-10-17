#version 450
// A forest impostor's shadow: the picture seen from the light (one view,
// set up by shadow_forest_impostor.vert), cut out by its coverage, at the
// depth of the leaves it holds plus a little (the bias a written depth no
// longer gets from the pipeline).

layout(location = 0) in vec3 inOrigin;
layout(location = 1) in vec3 inRay;
layout(location = 2) in vec4 inDepth;
layout(location = 3) flat in vec4 inView;
layout(location = 4) flat in vec4 inRight;
layout(location = 5) flat in vec4 inCenter;
layout(location = 6) flat in vec4 inAtlas;

// The card is the sphere's light-facing side: depth only grows from it, so
// the test still runs early. Shadow maps are always conventional depth.
layout(depth_greater) out float gl_FragDepth;

layout(set = 2, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 1) uniform sampler2D texNormal;

void main() {
    vec3 f = inView.xyz;
    vec3 up = cross(f, inRight.xyz);
    float dn = dot(inRay, f);
    float t = abs(dn) > 1e-8 ? dot(inCenter.xyz - inOrigin, f) / dn : 0.0;
    vec3 q = inOrigin + inRay * t - inCenter.xyz;
    vec2 uv = vec2(dot(q, inRight.xyz), -dot(q, up)) / inView.w * 0.5 + 0.5;
    float inset = inAtlas.w;
    bool inside = all(greaterThan(uv, vec2(inset))) && all(lessThan(uv, vec2(1.0 - inset)));
    vec2 st = (inAtlas.xy + clamp(uv, vec2(inset), vec2(1.0 - inset)) * inAtlas.z) / vec2(textureSize(texAlbedo, 0));
    // A level coarser than the footprint: a shadow's edge is softened by
    // its filter anyway, and the reads are its cost.
    float coverage = inside ? texture(texAlbedo, st, 1.0).a : 0.0;
    float depth = (texture(texNormal, st, 1.0).z * 2.0 - 1.0) * inView.w;
    float ts = (abs(dn) > 1e-8 ? t + depth / dn : t) + inRight.w;
    float z = max((inDepth.x + ts * inDepth.z) / (inDepth.y + ts * inDepth.w), gl_FragCoord.z);
    // The picture's coverage is the share of light its leaves stop: a
    // shadow texel keeps it against its own threshold, so a gappy crown's
    // shadow is a filtered half-shade, as its leaves' would be, rather than
    // solid. A gap is pushed to the far plane rather than discarded: a
    // discard turns the early depth test off for the draw, and these
    // casters overlap several deep.
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint v = p.x * 1973u + p.y * 9277u + 0x68e31da4u;
    v = (v ^ (v >> 16u)) * 0x7feb352du;
    v = (v ^ (v >> 15u)) * 0x846ca68bu;
    v ^= v >> 16u;
    float threshold = 0.15 + 0.8 * float(v & 0xffffu) * (1.0 / 65536.0);
    gl_FragDepth = coverage < threshold ? 1.0 : z;
}
