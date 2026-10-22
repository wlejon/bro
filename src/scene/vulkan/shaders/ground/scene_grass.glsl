// Grass: the cover a grass node grows over the land it stands on, the GPU
// half of scene/nodes/grass_node.h. Needs scene_camera.glsl (the camera, the
// wind field, the hashes and noise).
//
// This is the engine's half. What grows is the world's: where, how thickly
// and how dry, the land's surface module (scene_land_layer.glsl's
// LandSurface, read from the land's tiles here); its sward module
// (scene_sward_layer.glsl) says what the blades are, its herb module
// (scene_herb_layer.glsl) what low plants stand among them. Here are the
// node's data and the rings: no blade has any data of its own. A ring of
// cells around the camera is drawn as one draw; a cell's root is a hashed
// spot in it (an absolute cell of the ring's grid), where the module puts
// its blades' foot, and it holds several blades: a tuft near the camera,
// blades over the cell further off, fewer of them with distance and wider.
// The cover's plan (cover_plan.comp) lists the blades a ring draws, a blade
// an instance (grassBladeLive); the vertices build them (grassBlade). A far
// blade may carry a herb's bloom, standing for the herbs past their plants.
//
// A blade the draw does not want (outside its ring, off screen, thinned, not
// standing by the module's density) is left out of the plan's list; a
// caster, drawn over the ring's cells, culls it at every vertex by the same
// test.

#include "ground/scene_land_layout.h"

layout(set = 4, binding = 0) uniform GrassUBO {
    vec4 cells;      // x: the sward's blades' mean height (m), y: the ring's blade width (m), z: its growth a metre of distance, w: cell size (m) (the draw's cells are grassRect())
    vec4 ring;       // x: inner radius, y: outer radius, z: the cross-fade band at the inner edge, w: at the outer (m)
    vec4 blade;      // x: the node's blade width (m; cells.y over it is the ring's coarseness); yzw unused
    vec4 push;       // xy: world x, z of the influence map's corner; z: its cell (m); w: cells across
    vec4 pushRate;   // x: the bend's spring (Hz), y: its damping ratio, z unused, w: the trample's relax time (s)
    vec4 shape;      // x: blades an instance, y: vertices a blade, z: spread from the root (m), w: > 0 for a ring of tufts
    vec4 lod;        // x: thinning starts (m), y: the fewest blades kept (share), z: the share shadow casters keep, w: the eye's lift (m)
    // The herbs (scene_herb_layer.glsl): the low plants among the grass.
    vec4 herbInfo;   // x: 1 with a module, y: the herbs' cell (m), z: where the far plants end (m: the turf stands in past it), w: that hand-over's band (m)
    vec4 herbRing;   // a herb draw's ring: inner, outer, the band at the inner edge, at the outer
    vec4 herbShape;  // x: cards an instance, y: vertices a card, z: the ring (0 near, 1 mid, 2 far), w: where the whole plants end (m)
    // The turf's surface layer (scene_ground_layers.glsl; SurfaceLayers):
    vec4 layer;      // x: a level-0 tile's side (m), y: its texels a side, z: its border (texels), w: levels (0: none)
    vec4 layer2;     // x: a texel's most pixels, y: the page shift, z: page entries a side, w: a level-0 texel (m)
    // The land the node stands on (scene_land_layout.h's LandDraw, its eye
    // the frame's): where its tiles are read.
    vec4 land[LD_WORDS / 4];
    vec4 swardData[64];   // the sward module's data (GRASS_SWARD_DATA, scene_sward_layer.glsl's swardData)
    vec4 herbData[256];   // the herb module's data (GRASS_HERB_DATA, scene_herb_layer.glsl's herbData)
} grass;

// rg: the bend (x, z) at stamp time, b: the trample then, a: the stamp time.
layout(set = 4, binding = 2) uniform sampler2D grassPush;

// The land's tiles (scene_land.glsl): what grows where is its surface
// module's (scene_land_layer.glsl's LandSurface: the cover and the herbs).
layout(set = 4, binding = LG_HEIGHTS) uniform sampler2DArray landHeights;
layout(set = 4, binding = LG_COVERS) uniform sampler2DArray landCovers;
layout(set = 4, binding = LG_SURFACES) uniform sampler2DArray landSurfaces;
layout(set = 4, binding = LG_PAGE) uniform usampler2DArray landPage;
#define LAND_W(i) grass.land[i]
#define LAND_COVERS
#include "ground/scene_land.glsl"

// Sward density under which nothing grows (a few blades in a closed wood's
// shade are not seen).
const float GRASS_DENSITY_FLOOR = 0.06;

// The ground at `xz` as the grass takes it: in `field` its height, the
// sward's density, its dryness and its height multiplier, and in `under`
// the canopy over it and how thickly herbs grow (0 none .. 1 as their
// abundance has it), and in `normal` the ground's normal. Where herbs grow
// thick the grass gives them room, sparser and shorter (a meadow's forbs
// stand clear of it).
void grassGround(vec2 xz, out vec4 field, out vec2 under, out vec3 normal) {
    float h;
    vec4 surface, cover;
    landSampleCover(xz, landLevel(xz), h, normal, surface, cover);
    float herbs = grass.herbInfo.x > 0.5 ? surface.a : 0.0;
    float density = cover.x < GRASS_DENSITY_FLOOR ? 0.0 : cover.x * (1.0 - 0.5 * herbs);
    field = vec4(h, density, cover.y, cover.z * (1.0 - 0.45 * herbs));
    under = vec2(cover.w, surface.a);
}

// The floor's two channels: the canopy and the herbs' density.
vec2 grassFloorHerbsAt(vec2 xz) {
    vec4 f;
    vec2 fl;
    vec3 n;
    grassGround(xz, f, fl, n);
    return fl;
}

// The sward's blades' mean height (m), as the rings and the herbs take it.
float grassSwardHeight() { return grass.cells.x; }

// A hash of cell `c` and slot `k` in [0, 1): a blade's own draws.
float grassHash(ivec2 c, int k) {
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(k) * 2246822519u;
    h = (h ^ (h >> 15)) * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) / 4294967296.0;
}

#include "ground/scene_ground_noise.glsl"
#include "ground/scene_herb_layer.glsl"

// On the land, inside its extent.
bool grassInField(vec2 xz) { return all(lessThanEqual(abs(xz), vec2(landExtent()))); }

// How far `xz` is inside the land: 0 off it and at its edge, 1 from
// GRASS_FIELD_FADE m in. The blades and the herbs thin out over that margin
// (a whole instance at a time, as a ring hands over) and the turf
// (scene_ground.glsl) takes them as it does past their range, so the land's
// end shows no line in its cover.
const float GRASS_FIELD_FADE = 12.0;
float grassFieldInside(vec2 xz) {
    vec2 toEdge = landExtent() - abs(xz);
    return smoothstep(0.0, GRASS_FIELD_FADE, min(toEdge.x, toEdge.y));
}


// The movers' push at xz: the bend now, released since it was stamped; in
// `trample`, how flat the grass there is, and in `lie`, the way flattened
// blades lie (the push's direction when it was stamped). The release is
// GrassNode::bendRelease / trampleRelease (keep in step): the bend springs
// back as a damped spring, swinging a little past upright once; a trampled
// lane rises critically damped, a faint residue lasting a little longer.
const float GRASS_TRAMPLE_RESIDUE = 0.01;        // GrassNode::kTrampleResidue
const float GRASS_TRAMPLE_RESIDUE_RELAX = 2.0;   // GrassNode::kTrampleResidueRelax
vec2 grassInfluence(vec2 xz, out float trample, out vec2 lie) {
    trample = 0.0;
    lie = vec2(0.0);
    vec2 q = (xz - grass.push.xy) / (grass.push.z * grass.push.w);
    if (q.x <= 0.0 || q.y <= 0.0 || q.x >= 1.0 || q.y >= 1.0) return vec2(0.0);
    vec4 p = textureLod(grassPush, q, 0.0);
    float age = camera.windParams.x - p.a;   // the stamps are on the wind clock
    if (p.a <= 0.0 || age < 0.0) return vec2(0.0);
    float ra = age / grass.pushRate.w, rb = age / GRASS_TRAMPLE_RESIDUE_RELAX;
    trample = p.b * ((1.0 - GRASS_TRAMPLE_RESIDUE) * (1.0 + ra) * exp(-ra) + GRASS_TRAMPLE_RESIDUE * (1.0 + rb) * exp(-rb));
    lie = p.rg;
    float w = 6.2831853 * grass.pushRate.x, z = grass.pushRate.y;
    float wd = w * sqrt(1.0 - z * z);
    return p.rg * (exp(-z * w * age) * (cos(wd * age) + z * w / wd * sin(wd * age)));
}

#include "ground/scene_sward_layer.glsl"

// How much of the herbs' leaves and blooms past their plants shows among
// the grass (the module's herbFarMean counts them as if nothing stood round
// them; among the blades most leaves are hidden, as the plants' are, and
// some blooms): as much as the plants drawn up to their range show, so they
// hand over to the far blades and the turf unseen.
const float GRASS_HERB_LEAF_SEEN = 0.1;
const float GRASS_HERB_BLOOM_SEEN = 0.5;
// A far herb's bloom on a blade that carries one (grassSpeck): its dot's
// share of what the blade covers.
const float GRASS_SPECK_AREA = 0.8;
// A colour packed in one float (7 bits a channel, in sqrt, so the dark end
// keeps its steps), above 0: a varying the blade's vertices all share, so it
// reaches the fragment whole (grassUnpackColor).
float grassPackColor(vec3 c) {
    vec3 q = floor(sqrt(clamp(c, 0.0, 1.0)) * 127.0 + 0.5);
    return q.r * 16384.0 + q.g * 128.0 + q.b + 1.0;
}
vec3 grassUnpackColor(float p) {
    float q = floor(p + 0.5) - 1.0;
    vec3 c = vec3(floor(q / 16384.0), mod(floor(q / 128.0), 128.0), mod(q, 128.0)) / 127.0;
    return c * c;
}

#ifdef GRASS_SHADOW
// A caster's rectangle of cells: its first column and row and its width in
// cells, in the model matrix's first row (scene_mesh_drawer_grass.cpp: a
// draw a run of the tiles in view), and instance `id`'s cell in it.
ivec2 grassRectCell(int id) {
    vec3 rect = push.modelRows[0].xyz;
    int W = max(int(rect.z), 1);
    return ivec2(int(rect.x) + id % W, int(rect.y) + id / W);
}
#endif

// Blade `j` of cell `c` (the draw's cells, grass.cells.w a side), and
// whether the draw draws it: inside its ring, on the field, roughly on
// screen, kept by the thinning and standing by the module's density. The
// cover's plan (cover_plan.comp) runs it once a blade; a caster's vertices
// run it each.
bool grassBladeLive(ivec2 c, int j, out SwardCell s) {
    s.cell = c;
    s.h = vec4(sceneWindHash(c), sceneWindHash(c + ivec2(71, 13)), sceneWindHash(c + ivec2(-35, 29)),
               sceneWindHash(c + ivec2(17, -91)));
    s.f = vec4(0.0);
    s.ground = vec3(0.0, 1.0, 0.0);
    s.own = vec4(0.0);
    s.root = (vec2(c) + s.h.xy) * grass.cells.w;
    swardRoot(s);
    vec3 eye = camera.eyeWorld.xyz;
    // Across the ground and the eye's lift over it (lod.w): the rings scale
    // with the eye's height.
    s.d = length(vec2(length(s.root - eye.xz), grass.lod.w));

    // Thinning, first since it is the cheapest test: past lod.x the instance
    // keeps fewer of its blades, widened to keep the cover; shadow casters
    // keep fewer again.
    float keep = clamp(grass.lod.x / max(s.d, 1e-3), grass.lod.y, 1.0);
#ifdef GRASS_SHADOW
    keep *= grass.lod.z;
#endif
    s.kept = max(grass.shape.x * keep, 1.0);
    if (float(j) + grassHash(c, 4096 + j) * 0.999 >= s.kept + 0.5) return false;

    // The ring, handing over to its neighbours through a dithered band (a
    // whole instance at a time).
    float fin = grass.ring.x <= 0.0 ? 1.0 : smoothstep(grass.ring.x - grass.ring.z, grass.ring.x, s.d);
    float fout = 1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, s.d);
    if (s.h.w > fin || s.h.w > fout) return false;
    if (!grassInField(s.root) || s.h.w > grassFieldInside(s.root)) return false;
    vec2 under;
    vec3 ground;
    grassGround(s.root, s.f, under, ground);

    // Off screen: a generous box around the cell's middle.
    float H = grass.cells.x;
    vec4 cp = camera.viewProj * vec4(s.root.x - eye.x, s.f.r + 0.6 * H - eye.y, s.root.y - eye.z, 1.0);
    float m = cp.w * 0.15 + 2.5 * H + 0.5 + grass.shape.z;
    if (cp.w < -2.0 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;

    if (s.f.g <= 0.0 || s.f.a <= 0.0) return false;
    s.ground = ground;
    return grassHash(c, j * 4 + 3) <= swardDensity(s);
}

#ifdef SWARD_BUILD
// Blade `j` of cell `c` at blade-space vertex `v` (x across: -0.5, 0.5 or
// 0 at the tip; y 0 at the root .. 1 at the tip), as the module builds it.
// False when the draw does not draw that blade.
bool grassBlade(ivec2 c, int j, vec3 v, out SwardVertex o) {
    o.pos = vec3(0.0);
    o.normal = vec3(0.0, 1.0, 0.0);
    o.side = vec3(1.0, 0.0, 0.0);
    o.up = vec3(0.0, 1.0, 0.0);
    o.color = vec3(0.0);
    o.ao = 1.0;
    o.bulk = 0.0;
    o.far = 0.0;
    o.uv = vec2(0.0);
    o.info = vec4(0.0);
    o.carries = false;
    o.speck = 0.0;

    SwardCell s;
    if (!grassBladeLive(c, j, s)) return false;
    SwardRing r;
    r.tufted = grass.shape.w > 0.0;
    r.spread = grass.shape.z;
    r.width = grass.cells.y * (1.0 + s.d * grass.cells.z) * (grass.shape.x / s.kept);
    r.pixel = 2.0 / max(abs(camera.proj[1][1]) * camera.viewport.y, 1.0);
    r.coarse = grass.cells.y / max(grass.blade.x, 1e-6);
    r.verts = max(int(grass.shape.y + 0.5), 1);
    if (!swardBuild(s, j, r, v, o)) return false;

    // Past the herbs' plants a far blade stands for the herbs among the
    // grass as often as their cover seen from here has them (herbFarMean;
    // the turf, between the far blades and past them, does the same): their
    // leaves' mean colour, or one of their blooms by the patches there
    // (herbFarLocal), so the flowers the plants drew up to their range go
    // on as specks of their colour, patch by patch.
#ifndef GRASS_SHADOW
    if (o.carries && grass.herbInfo.x > 0.5) {
        vec3 eye = camera.eyeWorld.xyz;
        float tanView = grass.lod.w / max(length(s.root - eye.xz), 1e-3);
        float stand = 1.0 - herbPlantsDrawn(s.d, tanView);
        float u = grassHash(c, 6144 + j);
        if (u < stand) {
            vec2 fh = grassFloorHerbsAt(s.root);
            HerbMix hm = herbFarMean(fh.x, fh.y, tanView);
            if (fh.y > 0.0 && u < stand * hm.cover) {
                vec3 bloom, pick;
                float u2 = fract(u * 61.7 + grassHash(c, j * 4));
                float more = herbFarLocal(s.root, fh.x, fract(u2 * 37.3), 0.0, bloom, pick);
                float share = min(hm.bloomShare * more, 0.95);
                // A bloom is a dot over the blade's top (grassSpeck), not the
                // whole blade: a share of it about GRASS_SPECK_AREA, so as
                // many more blades carry one.
                float blooms = min(share * GRASS_HERB_BLOOM_SEEN / GRASS_SPECK_AREA, 0.9);
                if (u2 < blooms) o.speck = grassPackColor(pick);
                else if (u2 < blooms + (1.0 - hm.bloomShare) * GRASS_HERB_LEAF_SEEN) o.color = hm.leaf;
            }
        }
    }
#endif
    return true;
}
#endif

#ifdef SWARD_SHADE
// A far herb's bloom a blade carries (SwardVertex.speck): a round head at
// its card's top, as wide as about half the card and never under about a
// pixel across, cut per pixel. Returns the share of the pixel it covers.
float grassSpeck(vec2 uv) {
    vec2 fw = max(fwidth(uv), vec2(1e-5));
    float r = max(0.25 / fw.x, 0.6);   // its radius in pixels
    vec2 q = vec2(uv.x / fw.x, (1.0 - uv.y) / fw.y - r);
    return clamp(r + 0.5 - length(q), 0.0, 1.0);
}
#endif
