#version 450

// The sub-pixel resolve (scene/vulkan/post/pass_subpixel.h). A pixel whose
// surface a cut card's dither decided on detail finer than a pixel takes
// the 3x3 tent of its neighbours, as far as that share; a pixel of the
// ground those cards stand on, what their dither left showing, as far as
// its most marked neighbour; a solid surface (a leaf, a blade, a bloom a
// few pixels across) stays as it is. A neighbour counts when it lies at
// about the pixel's own depth, generously: what a far fan left showing is
// the ground a few metres on, part of the pixel's average as it would be
// under MSAA; a hill's crest over the next hill, a trunk before the meadow
// far behind it, or the sky is not.

#include "common/scene_subpixel_mark.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uColor;   // the opaque colour (a copy)
layout(set = 0, binding = 1) uniform sampler2D uMark;    // SceneTargets::mark
layout(set = 0, binding = 2) uniform sampler2D uDepth;   // the depth snapshot

layout(push_constant) uniform PushConsts {
    vec4 zw;   // the inverse projection's z row (x, y) and w row (z, w) on depth
} push;

// (reversed-Z: far, the sky, is 0)
float eyeDistance(float d) {
    return abs((push.zw.x * d + push.zw.y) / (push.zw.z * d + push.zw.w));
}

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    ivec2 hi = textureSize(uMark, 0) - 1;

    float centreMark = texelFetch(uMark, c, 0).r;
    bool ground = subpixelIsGround(centreMark);
    float shares[9];
    float most = 0.0;
    for (int i = 0; i < 9; ++i) {
        ivec2 q = clamp(c + ivec2(i % 3 - 1, i / 3 - 1), ivec2(0), hi);
        shares[i] = subpixelShare(texelFetch(uMark, q, 0).r);
        most = max(most, shares[i]);
    }
    if ((ground ? most : shares[4]) < 1.0 / 255.0) discard;
    float d0 = texelFetch(uDepth, c, 0).r;
    if (d0 <= 0.0) discard;
    float z0 = eyeDistance(d0);

    vec4 centre = texelFetch(uColor, c, 0);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    float blend = ground ? 0.0 : shares[4];
    for (int i = 0; i < 9; ++i) {
        ivec2 o = ivec2(i % 3 - 1, i / 3 - 1);
        ivec2 q = clamp(c + o, ivec2(0), hi);
        float d = texelFetch(uDepth, q, 0).r;
        if (d <= 0.0) continue;
        float z = eyeDistance(d);
        float near = 1.0 - smoothstep(0.25, 0.4, abs(z - z0) / min(z, z0));
        float w = near * float((2 - abs(o.x)) * (2 - abs(o.y)));
        sum += w * texelFetch(uColor, q, 0).rgb;
        weight += w;
        if (ground) blend = max(blend, near * shares[i]);
    }
    outColor = vec4(mix(centre.rgb, sum / weight, blend), centre.a);
}
