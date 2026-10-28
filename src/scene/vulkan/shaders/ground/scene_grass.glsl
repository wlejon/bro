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

// The most regions the turf's table holds (scene_mesh_drawer_grass.cpp's
// kTurfRegions).
#define GRASS_TURF_REGIONS 16

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
    // The cover's frame (coverRound and below): rows 0-2 its rotation into
    // the land's frame (xyz) and where its middle is there (w); row 3 the
    // eye in it (its map's x, its height over the datum, its map's z) and w
    // 1 on a round body (0: the land's own frame, the rest unused).
    vec4 frame[4];
    vec4 own;        // the region's bounds on its face's chart from the anchor's point there (face metres): lo x, lo z, hi x, hi z
    vec4 own2;       // x: its face, y: the hand-over band (m), z: the half side of the square about its middle wholly its own (m)
    // The turf's regions on a round body (scene_ground.glsl): x how many the
    // table below holds, y a face's cells a side, z the hand-over band (m),
    // w the turf's tiles' page layers a root.
    vec4 turf;
    // The cells about the eye whose frames are kept exact: each its rows as
    // `frame`'s, then its cell (face, x, z).
    vec4 turfRegions[GRASS_TURF_REGIONS * 5];
    // The land the node stands on (scene_land_layout.h's LandDraw, its eye
    // the frame's): where its tiles are read.
    vec4 land[LD_WORDS / 4];
    vec4 swardData[64];   // the sward module's data (GRASS_SWARD_DATA, scene_sward_layer.glsl's swardData)
    vec4 herbData[256];   // the herb module's data (GRASS_HERB_DATA, scene_herb_layer.glsl's herbData)
} grass;

// rg: the bend (x, z) at stamp time, b: the trample then, a: the stamp time.
// (The set's images are bare, read through its samplers: CS_*.)
#ifndef CUSTOM_SAMPLERS_DECLARED
#define CUSTOM_SAMPLERS_DECLARED
layout(set = 4, binding = CS_SAMPLERS) uniform sampler customSamplers[CS_SAMPLER_COUNT];
#endif
layout(set = 4, binding = 2) uniform texture2D grassPushImage;
#define grassPush sampler2D(grassPushImage, customSamplers[CS_LINEAR])

// The land's tiles (scene_land.glsl): what grows where is its surface
// module's (scene_land_layer.glsl's LandSurface: the cover and the herbs),
// and past the plants' range its canopy (the turf's fragments, mesh.frag).
layout(set = 4, binding = LG_HEIGHTS) uniform texture2DArray landHeightsImage;
layout(set = 4, binding = LG_COVERS) uniform texture2DArray landCoversImage;
layout(set = 4, binding = LG_SURFACES) uniform texture2DArray landSurfacesImage;
layout(set = 4, binding = LG_PAGE) uniform utexture2DArray landPageImage;
layout(set = 4, binding = LG_CANOPY) uniform texture2DArray landCanopiesImage;
#define landHeights sampler2DArray(landHeightsImage, customSamplers[CS_LINEAR])
#define landCovers sampler2DArray(landCoversImage, customSamplers[CS_LINEAR])
#define landSurfaces sampler2DArray(landSurfacesImage, customSamplers[CS_LINEAR])
#define landPage usampler2DArray(landPageImage, customSamplers[CS_NEAREST])
#define landCanopies sampler2DArray(landCanopiesImage, customSamplers[CS_LINEAR])
#define LAND_W(i) grass.land[i]
#define LAND_COVERS
#define LAND_CANOPY
#include "ground/scene_land.glsl"

// --- the cover's frame ---
//
// What the cover is keyed on (docs/scene-world-architecture.md §11, "The
// cover on the body"). On a flat body the land's frame: the cells, the eye
// and every point are its x, z and height. A round body has no flat chart,
// so the cover is laid out on REGIONS, the cells of the place frame's
// lattice: each region's own flat frame (its azimuthal map's x and z about
// its middle, and the height over the datum), the frame the place would
// have anchored there. A draw is a region's: its cells, its eye
// (coverEye), the roots and every position a module works out are that
// flat frame's, and the engine stands them on the body (coverToEye,
// coverMap). What stands at a point is the region's whose chart cell holds
// it (coverOwns), so a place's cover is the same wherever the place frame
// is anchored, and a re-anchor changes none of it.

// The frame the helpers below read: the draw's region (`grass.frame`), or
// the one the turf shades a point in (coverUse; scene_ground.glsl).
bool coverLocal = false;
vec4 coverLocalFrame[4];
vec4 coverRow(int i) { return coverLocal ? coverLocalFrame[i] : grass.frame[i]; }

bool coverRound() { return coverRow(3).w > 0.5; }

// The eye in the cover's frame.
vec3 coverEye() { return coverRound() ? coverRow(3).xyz : camera.eyeWorld.xyz; }

// What the body's curve adds to the region's flat point P (map x, z, its
// height y) in its frame (place.h's mapToLand less (x, y, z)), in series:
// P is within tens of kilometres of the region's middle, where a float's
// sin and cos of the angle would lose centimetres.
vec3 coverCurve(vec3 P) {
    float R = landRadius();
    float t2 = dot(P.xz, P.xz) / (R * R);            // the angle from the middle, squared
    float sincLess1 = t2 * (t2 / 120.0 - 1.0 / 6.0);  // sin t / t - 1
    float sinPerR = (1.0 + sincLess1) / R;            // sin t / r
    vec2 xz = P.xz * (sincLess1 + P.y * sinPerR);
    float hs2 = 0.25 * t2 * (1.0 - t2 / 12.0);        // sin^2(t / 2)
    return vec3(xz.x, -2.0 * hs2 * (P.y + R), xz.y);
}

// A direction in the cover's frame as the land's frame has it, and back.
vec3 coverTurn(vec3 d) {
    if (!coverRound()) return d;
    return vec3(dot(coverRow(0).xyz, d), dot(coverRow(1).xyz, d), dot(coverRow(2).xyz, d));
}
vec3 coverUnturn(vec3 d) {
    if (!coverRound()) return d;
    return d.x * coverRow(0).xyz + d.y * coverRow(1).xyz + d.z * coverRow(2).xyz;
}

// The land's map point under the cover's flat point `xz`.
vec2 coverMap(vec2 xz) {
    if (!coverRound()) return xz;
    vec3 p = vec3(xz.x, 0.0, xz.y);
    vec3 q = coverTurn(p + coverCurve(p)) + vec3(coverRow(0).w, coverRow(1).w, coverRow(2).w);
    return landMapOf(q);
}

// The flat point (map x, height over the datum, map z) of the frame with
// rows `rows` under land's-frame point `p` (place.h's landToMap there).
vec3 coverFlatOf(vec4 rows[4], vec3 p) {
    vec3 d = p - vec3(rows[0].w, rows[1].w, rows[2].w);
    vec3 q = d.x * rows[0].xyz + d.y * rows[1].xyz + d.z * rows[2].xyz;
    float R = landRadius();
    float rho = length(q.xz);
    vec2 xz = rho > 0.0 ? q.xz * (R * atan(rho, R + q.y) / rho) : vec2(0.0);
    // (|q + (0, R, 0)| - R, taken apart so a float of R loses nothing.)
    float h = (dot(q, q) + 2.0 * R * q.y) / (length(q + vec3(0.0, R, 0.0)) + R);
    return vec3(xz.x, h, xz.y);
}

// An offset `v` from the cover's eye (camera-relative in its flat frame)
// as the land's frame has it (camera-relative there): the curve's share
// of each end taken in series, so near the eye nothing is lost.
vec3 coverToEye(vec3 v) {
    if (!coverRound()) return v;
    vec3 E = coverEye();
    return coverTurn(v + (coverCurve(E + v) - coverCurve(E)));
}

// The ground's height over the datum at the cover's flat point `xz`.
float coverHeightAt(vec2 xz) { return landHeightAt(coverMap(xz)); }

// Whether the draw's region holds what stands at its flat point `xz`: on a
// round body the region whose chart cell holds it, its edges handed over
// through a band where each instance (its own `u`, 0..1) is taken a little
// way off, so the regions' patterns meet in a mingled band, not a line.
bool coverOwns(vec2 xz, float u) {
    if (!coverRound()) return true;
    float band = grass.own2.y;
    if (max(abs(xz.x), abs(xz.y)) < grass.own2.z - band) return true;
    vec2 j = (vec2(u, fract(u * 61.37 + 0.31)) - 0.5) * band;
    int f;
    vec2 off;
    if (!landChartOff(coverMap(xz + j), f, off)) return false;
    return f == int(grass.own2.x + 0.5) && all(greaterThanEqual(off, grass.own.xy)) && all(lessThan(off, grass.own.zw));
}

// --- the turf's regions ---
//
// The turf is shaded everywhere the land is, far past the draws' regions:
// a point takes the frame of the cell holding it, the table's (exact) for
// the cells about the eye, else worked out here, half a metre coarse at
// most, where a pixel is metres across. Through the band about a cell's
// edge the turf is the blend of the cells either side.

// A round body's lattice cell under map point `m`: its face, the cell, and
// where in it the point is (0..1 each way).
struct CoverCell {
    int face;
    ivec2 cell;
    vec2 at;
};
CoverCell coverCellOf(LandChart lc) {
    int f = lc.face;
    vec2 off = lc.off;
    bool near = lc.near;
    float cells = grass.turf.y;
    vec2 chart = off;
    if (near) {
        vec4 a = landFaceAnchor(f);
        chart += (a.xy + a.zw) * LAND_W(LD_TILES / 4).x;
    }
    vec2 L = chart / (landRadius() * (0.5 * 3.14159265 / cells)) + 0.5 * cells;
    CoverCell c;
    c.face = f;
    c.cell = clamp(ivec2(floor(L)), ivec2(0), ivec2(int(cells) - 1));
    c.at = clamp(L - vec2(c.cell), 0.0, 1.0);
    return c;
}
CoverCell coverCellOf(vec2 m) { return coverCellOf(landChartOf(m)); }

// Cell `c`'s frame, as `grass.frame`'s rows.
void coverCellFrame(CoverCell c, out vec4 rows[4]) {
    int n = int(grass.turf.x + 0.5);
    for (int i = 0; i < n; ++i) {
        vec4 id = grass.turfRegions[i * 5 + 4];
        if (int(id.x + 0.5) == c.face && ivec2(id.yz + 0.5) == c.cell) {
            for (int k = 0; k < 4; ++k) rows[k] = grass.turfRegions[i * 5 + k];
            return;
        }
    }
    // Its middle and axes (place.cpp's frameOf) in the land's frame, whose
    // up is the anchor's.
    float R = landRadius(), step = 0.5 * 3.14159265 / grass.turf.y;
    vec2 t = tan(-0.25 * 3.14159265 + (vec2(c.cell) + 0.5) * step);
    vec3 u = landFaceU(c.face);
    vec3 y = normalize(landFaceN(c.face) + t.x * u + t.y * landFaceV(c.face));
    vec3 x = normalize(u - dot(u, y) * y);
    vec3 z = cross(x, y);
    vec3 o = R * (y - vec3(0.0, 1.0, 0.0));
    rows[0] = vec4(x.x, y.x, z.x, o.x);
    rows[1] = vec4(x.y, y.y, z.y, o.y);
    rows[2] = vec4(x.z, y.z, z.z, o.z);
    rows[3] = vec4(coverFlatOf(rows, landEye3()), 1.0);
}

// The cells the turf at land's-frame point `p` is shaded in and how much
// each: the one holding it, and in the band about its edges the one across
// each edge near (found a nudge over it, on this face or the next), mixed
// bilinearly. Returns how many. `lc` is p's map point's chart place
// (landChartOf(landMapOf(p))).
int coverTurfCells(vec3 p, LandChart lc, out CoverCell cells[4], out float w[4]) {
    vec2 m = landMapOf(p);
    cells[0] = coverCellOf(lc);
    float side = landRadius() * (0.5 * 3.14159265 / grass.turf.y);
    float hb = 0.5 * grass.turf.z;
    // Its distance in from the nearer edge each way (chart metres), and
    // that edge's side.
    vec2 depth = min(cells[0].at, 1.0 - cells[0].at) * side;
    vec2 toward = mix(vec2(-1.0), vec2(1.0), greaterThanEqual(cells[0].at, vec2(0.5)));
    // Its own share each way: a half at the edge, all of it half the band in.
    vec2 own = 0.5 + 0.5 * smoothstep(0.0, hb, depth);
    w[0] = own.x * own.y;
    int n = 1;
    bvec2 across = lessThan(depth, vec2(hb));
    if (!any(across)) return n;
    vec3 up = landUp(m);
    int f = cells[0].face;
    vec3 tu = normalize(landFaceU(f) - dot(landFaceU(f), up) * up);
    vec3 tv = normalize(landFaceV(f) - dot(landFaceV(f), up) * up);
    vec3 nx = tu * (toward.x * (2.0 * depth.x + 1.0));
    vec3 nz = tv * (toward.y * (2.0 * depth.y + 1.0));
    if (across.x) {
        cells[n] = coverCellOf(landMapOf(p + nx));
        w[n++] = (1.0 - own.x) * own.y;
    }
    if (across.y) {
        cells[n] = coverCellOf(landMapOf(p + nz));
        w[n++] = own.x * (1.0 - own.y);
    }
    if (all(across)) {
        cells[n] = coverCellOf(landMapOf(p + nx + nz));
        w[n++] = (1.0 - own.x) * (1.0 - own.y);
    }
    return n;
}

// Shades with the cover's helpers in the frame of `rows` (until coverDone).
void coverUse(vec4 rows[4]) {
    coverLocal = true;
    for (int k = 0; k < 4; ++k) coverLocalFrame[k] = rows[k];
}
void coverDone() { coverLocal = false; }

// Sward density under which nothing grows (a few blades in a closed wood's
// shade are not seen), reached by a ramp from twice it: a cut there drew a
// hard line where a climate thins the sward (a planet's grass line along
// the hills seen from its height).
const float GRASS_DENSITY_FLOOR = 0.06;

// The ground at `xz` (the cover's frame) as the grass takes it: in `field`
// its height, the
// sward's density, its dryness and its height multiplier, and in `under`
// the canopy over it and how thickly herbs grow (0 none .. 1 as their
// abundance has it), and in `normal` the ground's normal. Where herbs grow
// thick the grass gives them room, sparser and shorter (a meadow's forbs
// stand clear of it).
void grassGround(vec2 xz, out vec4 field, out vec2 under, out vec3 normal, out vec3 albedo) {
    float h;
    vec4 surface, cover;
    vec2 m = coverMap(xz);
    landSampleCover(m, landLevel(m), h, normal, surface, cover);
    normal = coverUnturn(normal);   // (in the cover's frame)
    float herbs = grass.herbInfo.x > 0.5 ? surface.a : 0.0;
    float density = cover.x * smoothstep(GRASS_DENSITY_FLOOR, 2.0 * GRASS_DENSITY_FLOOR, cover.x) * (1.0 - 0.5 * herbs);
    field = vec4(h, density, cover.y, cover.z * (1.0 - 0.45 * herbs));
    under = vec2(cover.w, surface.a);
    albedo = surface.rgb;
}
void grassGround(vec2 xz, out vec4 field, out vec2 under, out vec3 normal) {
    vec3 albedo;
    grassGround(xz, field, under, normal, albedo);
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
bool grassInField(vec2 xz) { return all(lessThanEqual(abs(xz), vec2(landReach()))); }

// How far `xz` is inside the land: 0 off it and at its edge, 1 from
// GRASS_FIELD_FADE m in. The blades and the herbs thin out over that margin
// (a whole instance at a time, as a ring hands over) and the turf
// (scene_ground.glsl) takes them as it does past their range, so the land's
// end shows no line in its cover.
const float GRASS_FIELD_FADE = 12.0;
float grassFieldInside(vec2 xz) {
    vec2 toEdge = landReach() - abs(xz);
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
    // (The map is the land's frame's, as the movers are.)
    vec2 q = (coverMap(xz) - grass.push.xy) / (grass.push.z * grass.push.w);
    if (q.x <= 0.0 || q.y <= 0.0 || q.x >= 1.0 || q.y >= 1.0) return vec2(0.0);
    vec4 p = textureLod(grassPush, q, 0.0);
    if (coverRound()) {
        vec3 b = vec3(p.r, 0.0, p.g);
        p.rg = vec2(dot(vec3(grass.frame[0].x, grass.frame[1].x, grass.frame[2].x), b),
                    dot(vec3(grass.frame[0].z, grass.frame[1].z, grass.frame[2].z), b));
    }
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
    vec3 eye = coverEye();
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
    if (!coverOwns(s.root, grassHash(c, 8191))) return false;
    vec2 under;
    vec3 ground;
    grassGround(s.root, s.f, under, ground);

    // Off screen: a generous box around the cell's middle.
    float H = grass.cells.x;
    vec4 cp = camera.viewProj * vec4(coverToEye(vec3(s.root.x - eye.x, s.f.r + 0.6 * H - eye.y, s.root.y - eye.z)), 1.0);
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
    // Stood on the body: the module's flat frame turned into the land's.
    o.pos = coverToEye(o.pos);
    o.normal = coverTurn(o.normal);
    o.side = coverTurn(o.side);
    o.up = coverTurn(o.up);

    // Past the herbs' plants a far blade stands for the herbs among the
    // grass as often as their cover seen from here has them (herbFarMean;
    // the turf, between the far blades and past them, does the same): their
    // leaves' mean colour, or one of their blooms by the patches there
    // (herbFarLocal), so the flowers the plants drew up to their range go
    // on as specks of their colour, patch by patch.
#ifndef GRASS_SHADOW
    if (o.carries && grass.herbInfo.x > 0.5) {
        vec3 eye = coverEye();
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
