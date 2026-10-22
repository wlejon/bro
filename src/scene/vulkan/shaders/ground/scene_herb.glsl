// Herbs: the engine's half of a grass node's herb plants (what they are is
// the world's module, scene_herb_layer.glsl). Needs scene_camera.glsl and
// scene_grass.glsl; under HERB_BUILD (mesh_herb.vert) the module's build,
// with scene_vegetation.glsl ahead of it for the wind.
//
// No plant has data of its own. A plant is a cell of the herbs' grid; the
// module's herbStand says which of its plants grows there, if any, and the
// cell's hashes the rest. The cover's plan (cover_plan.comp) lists the
// cells of a ring that hold a plant it draws, a plant an instance; the
// ring's mesh is `cards` strips of a few segments an instance (x across
// -0.5..0.5, y along 0..1), which the module's herbBuild makes into the
// plant's cards. The far herb ring draws fewer cards a plant, a quad each.

#include "ground/scene_cover_far.glsl"

// The plant of cell `c` of the herbs' grid, if the draw draws one: inside
// its ring, kept by the far ring's thinning, on the field, roughly on
// screen, and the module standing one there. The cover's plan
// (cover_plan.comp) runs it once a cell. Sets all of `p` but its root and
// ground (herbVertex's).
bool herbPlantLive(ivec2 c, out HerbPlant p) {
    p.cell = c;
    p.h = vec4(grassHash(c, 101), grassHash(c, 102), grassHash(c, 103), grassHash(c, 104));
    p.root = vec3(0.0);
    p.ground = vec3(0.0, 1.0, 0.0);
    p.floorAt = vec2(0.0);
    p.farWiden = 1.0;
    p.variety = -1;
    p.rootXZ = (vec2(c) + p.h.xy) * grass.herbInfo.y;
    vec3 eye = camera.eyeWorld.xyz;
    p.d = length(vec2(length(p.rootXZ - eye.xz), grass.lod.w));

    // The ring, handing over through a dithered band, a plant at a time.
    float fin = grass.herbRing.x <= 0.0 ? 1.0 : smoothstep(grass.herbRing.x - grass.herbRing.z, grass.herbRing.x, p.d);
    float fout = 1.0 - smoothstep(grass.herbRing.y - grass.herbRing.w, grass.herbRing.y, p.d);
    if (p.h.w > fin || p.h.w > fout) return false;
    // The far ring keeps the plants the far tier would (scene_cover_far.glsl:
    // as many as the screen holds, each widened to keep their cover), so
    // past it the same plants go on as the tier's elements. (Kept with the
    // ground face on first: the view meeting it at a slant only thins more.)
    bool far = grass.herbShape.z > 1.5;
    if (far && coverKept(c, coverLevel(p.d, 1.0)) <= 0.0) return false;
    if (!grassInField(p.rootXZ) || p.h.w > grassFieldInside(p.rootXZ)) return false;

    // Off screen: a box round the cell as tall as the tallest herb.
    float groundY = grassFieldAt(p.rootXZ).r;
    vec4 cp = camera.viewProj * vec4(p.rootXZ.x - eye.x, groundY + 0.5 - eye.y, p.rootXZ.y - eye.z, 1.0);
    float m = cp.w * 0.15 + 2.2;
    if (cp.w < -2.5 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;
    if (far) {
        vec3 toEye = normalize(vec3(eye.x - p.rootXZ.x, eye.y - groundY, eye.z - p.rootXZ.y));
        float lam = coverLevel(p.d, dot(grassGroundNormal(p.rootXZ), toEye));
        if (coverKept(c, lam) <= 0.0) return false;
        p.farWiden = coverWiden(lam);
    }

    p.floorAt = grassFloorHerbsAt(p.rootXZ);
    p.variety = herbStand(c, p.rootXZ, p.floorAt);
    return p.variety >= 0 && p.variety < HERB_VARIETIES;
}

#ifdef HERB_BUILD
// The plant of cell `c` (one the plan listed) at vertex `vid` of the ring
// mesh (card vid / vertices a card), card-space vertex `v`. False when the
// module leaves that card out.
bool herbVertex(ivec2 c, int vid, vec3 v, out HerbVertex o) {
    o.pos = vec3(0.0);
    o.normal = vec3(0.0, 1.0, 0.0);
    o.side = vec3(1.0, 0.0, 0.0);
    o.ground = vec3(0.0, 1.0, 0.0);
    o.tint = vec3(1.0);
    o.uv = vec2(0.0);
    o.ao = 1.0;
    o.bulk = 0.3;
    o.seed = 0.0;
    o.id = vec3(0.0);
    o.a = vec4(0.0);
    o.b = vec4(0.0);
    o.c = vec2(0.0);

    HerbPlant p;
    if (!herbPlantLive(c, p)) return false;
    vec3 eye = camera.eyeWorld.xyz;
    p.root = vec3(p.rootXZ.x - eye.x, grassFieldAt(p.rootXZ).r - eye.y, p.rootXZ.y - eye.z);
    p.ground = grassGroundNormal(p.rootXZ);
    o.ground = p.ground;
    int cards = int(grass.herbShape.x + 0.5);
    int verts = max(int(grass.herbShape.y + 0.5), 1);
    return herbBuild(p, vid / verts, cards, verts, v, o);
}
#endif
