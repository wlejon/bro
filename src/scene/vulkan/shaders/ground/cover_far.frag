#version 450
// The cover's far tier (mesh_cover_far.vert): an element's quad cut to what
// it stands for, lit as the sward it is part of. A tuft is a fan of blade
// tips over a darker base; a herb is a mound of its leaves dotted with its
// blooms. Its cover of a pixel is kept as that share of the samples
// (scene_cover_mask.glsl), so its edges blend rather than crawl.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) flat in vec4 inBody;
layout(location = 3) flat in vec4 inBloom;
layout(location = 4) flat in vec4 inGround;

layout(location = 0) out vec4 outColor;
// (mesh.frag's: the opaque scope's sub-pixel mark.)
layout(location = 1) out float outMark;
#include "common/scene_subpixel_mark.glsl"
#ifdef SCENE_INDIRECT_OUTPUT
layout(location = 2) out vec4 outIndirect;
#endif

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"
#include "common/scene_mesh_push.glsl"
#include "common/scene_cover_mask.glsl"

const int COVER_FAR_LOBES = 5;   // a herb clump's

float coverFarHash(float x) { return fract(sin(x * 127.1 + inGround.w * 311.7) * 43758.5453); }

void main() {
    uint flags = meshFlags();
    vec2 uv = inUV;
    bool tuft = inBody.w < 0.5;
    // How far a pixel spans in uv, for the edges' cover.
    vec2 fw = max(fwidth(uv), vec2(1e-4));

    // Its detail (a tuft's tips, a herb's lobes and blooms) is drawn only
    // as far as it spans a pixel or two: finer, it is its mean, as a pixel
    // sees it, or it crawls as the element moves over the pixels.
    float inside;            // signed distance in from the edge, in pixels
    float lobeShade = 1.0;   // a herb's lobe's own tone
    float bloomShare = 0.0;  // the share of what is seen here that is a bloom
    float height;            // 0 at the root .. 1 at the top, for the shade within
    if (tuft) {
        // A few blades' tips, each its own height, the tuft narrowing
        // toward them (their mean: the tips' mean height less their taper's).
        float strands = 5.0;
        float seen = smoothstep(1.0, 2.5, 2.0 / (strands * fw.x));
        float s = (uv.x * 0.5 + 0.5) * strands;
        float k = floor(s);
        float tip = mix(0.725, mix(0.55, 1.0, coverFarHash(k)), seen);
        float f = fract(s) - 0.5;
        float top = tip - 0.6 * f * f * seen;
        float edgeX = 1.0 - 0.35 * clamp(uv.y, 0.0, 1.0);
        inside = min((top - uv.y) / fw.y, (edgeX - abs(uv.x)) / fw.x);
        height = clamp(uv.y / tip, 0.0, 1.0);
    } else {
        // A clump of lobes (its leaves' masses), each its own size and
        // shade and lit at its top, its blooms dots in a jittered grid.
        inside = -1e4;
        lobeShade = 1.0;
        vec2 pxs = 1.0 / fw;   // pixels a unit of uv
        float lobeSeen = smoothstep(1.5, 4.0, 0.36 * min(pxs.x, pxs.y * 0.75));
        for (int i = 0; i < COVER_FAR_LOBES; ++i) {
            float fi = float(i);
            vec2 c = vec2((coverFarHash(fi + 0.5) * 2.0 - 1.0) * 0.55,
                          0.22 + 0.5 * coverFarHash(fi + 7.5) * (1.0 - 0.5 * abs(coverFarHash(fi + 0.5) * 2.0 - 1.0)));
            float r = mix(0.28, 0.45, coverFarHash(fi + 13.5));
            vec2 q = (uv - c) / vec2(1.0, 0.75);
            float din = (r - length(q)) * min(pxs.x, pxs.y * 0.75);
            if (din > inside) {
                inside = din;
                lobeShade = mix(0.72, 1.12, coverFarHash(fi + 21.5)) * mix(0.8, 1.1, clamp((q.y / r) * 0.5 + 0.5, 0.0, 1.0));
            }
        }
        // (Their mean: the hashed shade's, 0.92, times the light down a
        // lobe's, 0.95.)
        lobeShade = mix(0.874, lobeShade, lobeSeen);
        height = clamp(uv.y, 0.0, 1.0);
        vec2 g = (uv * vec2(0.5, 1.0) + vec2(0.5, 0.0)) * 3.0;
        vec2 gi = floor(g);
        float hb = coverFarHash(gi.x * 7.0 + gi.y * 13.0 + 3.0);
        vec2 c = vec2(coverFarHash(gi.x * 3.0 + gi.y * 17.0 + 1.0), coverFarHash(gi.x * 11.0 + gi.y * 5.0 + 2.0));
        float dotR = 0.32;
        float chance = clamp(inBloom.w * (0.6 + 0.8 * height), 0.0, 1.0);
        bool dot_ = hb < chance && length(fract(g) - (0.25 + 0.5 * c)) < dotR;
        vec2 gw = max(fwidth(g), vec2(1e-4));
        float dotSeen = smoothstep(1.5, 3.0, 2.0 * dotR / max(gw.x, gw.y));
        bloomShare = mix(chance * 3.14159 * dotR * dotR, dot_ ? 1.0 : 0.0, dotSeen);
    }
    float cover = clamp(inside + 0.5, 0.0, 1.0);
    if (cover < 1.0 / 255.0) discard;
    int mask = coverSampleMask(cover, (flags & MESH_MSAA) != 0u, uint(inGround.w * 16777216.0));
    if (mask == 0) discard;
    gl_SampleMask[0] = mask;

    // Its shade up its height likewise: an element a pixel or two tall (a
    // tuft widened to keep the cover far off) is its mean tone, or each is
    // a dark bar at its root.
    height = mix(0.5, height, smoothstep(1.5, 4.0, 1.0 / fw.y));

    vec3 worldPos = inWorldPos;
    float camDist = length(worldPos);
    vec3 view = normalize(-worldPos);
    vec3 gn = normalize(inGround.xyz);

    // The stand's own shade: dark at its root, lit at its top. Blooms by
    // their share of what is seen.
    // An element under a dozen pixels tall is the sward's tone where it
    // stands, as the turf under it is: its root's dark and its top's light
    // are a gradient every element repeats alike, so a field of them is a
    // field of stamped marks darker than the turf between (biomes' glade's
    // far hillside, elements 4 to 10 pixels tall, at golden hour).
    float tall = smoothstep(4.0, 16.0, 1.0 / fw.y);
    vec3 baseColor = mix(inBody.rgb * mix(1.0, mix(0.7, 1.05, height) * lobeShade, tall), inBloom.rgb, bloomShare);
    float ao = mix(mix(1.0, mix(0.45, 1.0, smoothstep(0.0, 0.9, height)), tall), 1.0, bloomShare);

    SceneSurface s;
    s.position = worldPos;
    s.normal = normalize(gn + mix(0.35, 1.0, bloomShare) * view);
    s.view = view;
    s.camDist = camDist;
    s.baseColor = baseColor;
    s.metallic = 0.0;
    s.roughness = mix(0.85, 0.6, bloomShare);
    s.receivesShadow = (flags & MESH_RECEIVES_SHADOW) != 0u;
    s.twoSided = true;
    s.subsurface = push.extra.y;
    s.translucency = mix(0.6, 0.3, bloomShare);
    s.thinNormal = view;
    // Leaves and blades lit as the sward they stand in; blooms as
    // themselves, on top.
    s.sward = vec4(gn, 1.0 - bloomShare);
    s.landShade = 1.0;

    SceneProbe probe = sceneProbe(worldPos);
    vec3 indirect = sceneAmbient(s, probe) * ao;
    vec3 color = sceneDirectLight(s, probe) * mix(1.0, mix(0.6, 1.0, height), tall) + indirect;

    float alpha = 1.0;
    if ((flags & MESH_REFLECTANCE) != 0u) {
        float smooth_ = 1.0 - s.roughness;
        alpha = 0.04 * smooth_ * smooth_;
    }
    SceneAir air = sceneAir(worldPos, camDist);
    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    alpha = mix(alpha, 0.0, air.fade);

    outColor = vec4(min(color, vec3(SCENE_HDR_MAX)), alpha);    // Each element stands for a few pixels (its tips, lobes and blooms fade
    // to their means as they near one): its part-covered edge pixels are
    // the dither's all-or-nothing ones, for the sub-pixel resolve to blend.
    outMark = subpixelMark(subpixelDecided(cover));
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(min(indirect, vec3(SCENE_HDR_MAX)), 0.0);
#endif
}
