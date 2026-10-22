#version 450
// The cover's far tier (scene_cover_far.glsl): an instance an element of a
// level's list in the cover's plan (scene_cover_plan.glsl, the entry's slot
// its kind), each a quad standing at its root and facing the eye, as wide as
// the element's widening keeps the cover and as tall as what grows there.
// The vertices are the first four of the ring mesh's first card (x across
// -0.5..0.5, row by vertex index). Drawn with cover_far.frag, which cuts the
// quad to a tuft of the sward or a herb's mound with its blooms.

layout(location = 0) in vec3 inPos;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outUV;          // x across (-1..1), y up the element (0 its root .. 1 its top)
layout(location = 2) flat out vec4 outBody;   // rgb the leaves' colour, w the kind (0 a tuft, else a herb)
layout(location = 3) flat out vec4 outBloom;  // rgb the blooms' colour, w their share of the mound
layout(location = 4) flat out vec4 outGround; // xyz the ground's normal, w the element's seed

#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#include "ground/scene_grass.glsl"
#include "ground/scene_cover_far.glsl"
#include "ground/scene_cover_plan.glsl"

// How far below its root a quad starts, as a share of its height (so a
// slope falling away in front of it shows no gap under it).
const float COVER_FAR_SINK = 0.2;

void main() {
    uvec2 entry = coverList.entries[gl_InstanceIndex];
    CoverFar e;
    if (!coverFarLive(coverCell(entry), int(grass.herbShape.x + 0.5), coverSlot(entry), e)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outBody = vec4(0.0);
        outBloom = vec4(0.0);
        outGround = vec4(0.0);
        return;
    }
    vec4 h2 = vec4(grassHash(coverRep(coverCell(entry), int(grass.herbShape.x + 0.5)), 9300),
                   e.h.z, fract(e.h.x * 7.13 + e.h.y), 0.0);
    float cell = grass.herbInfo.y;
    float ht, wd;
    vec3 body, bloom = vec3(0.0);
    float bloomShare = 0.0;
    if (e.kind == 0) {
        // A tuft of the sward: the module's (scene_sward_layer.glsl's
        // swardFarLook).
        SwardFarLook look = swardFarLook(coverFarSward(e, coverRep(coverCell(entry), int(grass.herbShape.x + 0.5))), h2);
        ht = look.height;
        wd = look.width;
        body = look.body;
    } else {
        // A herb: the module's (scene_herb_layer.glsl's herbFarLook).
        HerbFarLook look = herbFarLook(e.kind - 1, h2);
        ht = look.height * look.size;
        wd = max(look.width, cell) * look.size;
        body = look.body;
        bloom = look.bloom;
        bloomShare = clamp(look.bloomShare, 0.0, 1.0);
    }
    // Widened to keep the cover it stands for, shrinking as it is let go.
    wd *= coverWiden(e.lam) * e.size;
    ht *= mix(1.0, 0.4, 1.0 - e.size);

    // Upright where the eye looks across the ground, a blob as wide as it is
    // long where it looks down on it: its length along the screen's up runs
    // from the width (from above) to the height (seen across).
    vec3 eye = camera.eyeWorld.xyz;
    vec3 root = vec3(e.root.x - eye.x, e.f.r - eye.y, e.root.y - eye.z);
    float dist = max(length(root), 1e-3);
    vec3 toEye = -root / dist;
    vec3 side = cross(vec3(0.0, 1.0, 0.0), toEye);
    side = dot(side, side) > 1e-6 ? normalize(side) : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(toEye, side);
    float across = clamp(up.y, 0.0, 1.0);
    // (Never under 0.4 of its width: widened far off to keep the cover, it
    // would be a flat bar, the ground in slivers between, where the sward
    // it stands for is a patch.)
    float len = mix(wd * 0.8, max(ht, 0.4 * wd), across);
    float x = inPos.x > 0.0 ? 1.0 : -1.0;
    float y = float(gl_VertexIndex >> 1);
    float along = mix(-COVER_FAR_SINK, 1.0, y) - (1.0 - across) * 0.5;
    vec3 p = root + side * (x * 0.5 * wd) + up * (along * len);
    // Drawn a little toward the eye (along its own line, so it stays where
    // it is on screen): the ground it stands on does not cut it.
    p *= 1.0 - min((0.5 * ht + 0.003 * dist) / max(length(p), 1e-3), 0.5);

    outWorldPos = p;
    gl_Position = camera.viewProj * vec4(p, 1.0);
    outUV = vec2(x, mix(-COVER_FAR_SINK, 1.0, y));
    outBody = vec4(body, float(e.kind));
    outBloom = vec4(bloom, bloomShare);
    outGround = vec4(e.gn, h2.x);
}

// The world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl), after
// all they may use.
#include "ground/sward_module.glsl"
#include "ground/herb_module.glsl"
