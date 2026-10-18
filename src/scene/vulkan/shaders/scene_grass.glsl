// Grass: dense procedural blades, the GPU half of scene/grass_node.h. Needs
// scene_camera.glsl (the camera, the wind field, the hashes and noise).
//
// No blade has any data of its own. A ring of cells around the camera is
// drawn as one instanced draw. An instance is a cell: its root is a hashed
// spot in the cell (the draw's rectangle of absolute cells), gathered toward
// its tussock where the grass grows in bunches, and it draws several blades
// (the ring mesh is that many copies of the blade, so the vertex index says
// which): a tuft's tillers rising from a small crown near the camera, blades
// over the cell further off. The ground there (the node's field texture), the
// clump noise and a few shared noises (bunches, flowering patches) do the
// rest. Everything after is per vertex:
//
//   - a blade is a leaf, a dead leaf or a flowering culm, by its hash and
//     the share of each where it grows (GrassNode::Params tufts, flowering,
//     dead), and has an age: young leaves stand up from the crown's middle,
//     older and longer ones arch out from it and droop under their own
//     weight, a few lie lodged, the dead ones lie at the foot;
//   - its shape is its tilt from upright along its length (a lean at the
//     root, its rest curvature, and the bend of the wind and the movers,
//     added as tilt vectors so nothing passes through a flip), integrated up
//     the blade; it tapers to a fine point and twists a little about its
//     length;
//   - the wind is the scene's field: the steady lean, each blade's own sway,
//     and the travelling gust bands of sceneWindGustAt, each blade answering
//     by its own flex;
//   - near the eye it is lit as its own folded face (mesh.frag's SCENE_GRASS
//     build), the sun glowing through it from behind; with distance it is
//     lit more and more as the ground it covers (vegShade.xyz), so a meadow
//     reads as one surface;
//   - its colour runs from lush to sun-dried with the field's dry mask, paler
//     at its sheath, dark at the root and lifted where a gust bends it.
//
// A blade the draw does not want (outside its ring, off screen, thinned by
// the density) is culled at every vertex by the same instance-only test, so
// no triangle of it survives.

#define GRASS_HERB_MAX 12      // GrassNode::kHerbMax
#define GRASS_HERB_RECORD 13   // vec4s a species (broflora::kHerbRecordFloats / 4)
#define GRASS_HERB_MEAN 5      // GrassNode::kHerbMeanSteps (kHerbMeanFloats / 4 vec4s each)

layout(set = 4, binding = 0) uniform GrassUBO {
    vec4 cells;      // x: tufts (0 sward .. 1 bunch grass), y: flowering share, z: dead share; w: cell size (m) (the draw's cells are grassRect())
    vec4 ring;       // x: inner radius, y: outer radius, z: the cross-fade band at the inner edge, w: at the outer (m)
    vec4 field;      // xy: world x, z of texel (0, 0)'s centre; z: texel spacing (m); w: the node's blade width (m; blade.y over it is the ring's coarseness)
    vec4 fieldSize;  // cols, rows, 1 / cols, 1 / rows
    vec4 blade;      // x: mean height (m), y: width (m), z: min ground normal y, w: wind response
    vec4 lush;       // rgb lush colour, w: ground-normal weight
    vec4 dry;        // rgb sun-dried colour, w: clump frequency (1/m)
    vec4 look;       // x: root shade, y: gust sheen, z: height variety, w: width growth per metre of distance
    vec4 push;       // xy: world x, z of the influence map's corner; z: its cell (m); w: cells across
    vec4 pushRate;   // x: the bend's spring (Hz), y: its damping ratio, z: 1 with a floor texture, w: the trample's relax time (s)
    vec4 shape;      // x: blades an instance, y: vertices a blade, z: spread from the root (m), w: > 0 for a ring of tufts
    vec4 lod;        // x: thinning starts (m), y: the fewest blades kept (share), z: the share shadow casters keep, w: the eye's lift (m)
    // The herbs (scene_herb_mix.glsl): the low plants among the grass.
    vec4 herbInfo;   // x: species, y: the herbs' cell (m), z: where the far plants end (m: the turf stands in past it), w: that hand-over's band (m)
    vec4 herbRing;   // a herb draw's ring: inner, outer, the band at the inner edge, at the outer
    vec4 herbShape;  // x: cards an instance, y: vertices a card, z: the ring (0 near, 1 mid, 2 far), w: where the whole plants end (m)
    vec4 herbs[GRASS_HERB_MAX * GRASS_HERB_RECORD];   // GrassNode::kHerbMax species of broflora's herbRecord
    // The herbs' mean over their colonies under a canopy of 0, 1/4 .. 1
    // (GrassNode::herbMean): (cover exponent from above, leaf rgb), (the
    // blooms' share, bloom rgb), (exponent from the side, the blooms' share
    // of that, the colonies' share of the ground, the leaves' top).
    vec4 herbMean[GRASS_HERB_MEAN * 3];
} grass;

const float GRASS_REFERENCE_SPEED = 2.5;   // m/s: the steady wind blades flutter in as authored

// r: ground height, g: density (0..1), b: dryness (0 lush .. 1 dry), a: height multiplier.
layout(set = 4, binding = 1) uniform sampler2D grassField;
// rg: the bend (x, z) at stamp time, b: the trample then, a: the stamp time.
layout(set = 4, binding = 2) uniform sampler2D grassPush;
// The forest floor on the field's grid: r how closed the canopy overhead is,
// 0 open .. 1 closed (litter; the herbs' light); g how thickly herbs grow
// there (0 none .. 1 as their abundance has it).
layout(set = 4, binding = 3) uniform sampler2D grassFloor;

vec4 grassFieldAt(vec2 xz) {
    vec2 uv = ((xz - grass.field.xy) / grass.field.z + 0.5) * grass.fieldSize.zw;
    return textureLod(grassField, uv, 0.0);
}

float grassFloorAt(vec2 xz) {
    vec2 uv = ((xz - grass.field.xy) / grass.field.z + 0.5) * grass.fieldSize.zw;
    return textureLod(grassFloor, uv, 0.0).r;
}

// The floor's two channels: the canopy and the herbs' density.
vec2 grassFloorHerbsAt(vec2 xz) {
    vec2 uv = ((xz - grass.field.xy) / grass.field.z + 0.5) * grass.fieldSize.zw;
    return textureLod(grassFloor, uv, 0.0).rg;
}

#include "scene_herb_mix.glsl"

bool grassInField(vec2 xz) {
    vec2 q = (xz - grass.field.xy) / grass.field.z;
    return q.x >= 0.0 && q.y >= 0.0 && q.x <= grass.fieldSize.x - 1.0 && q.y <= grass.fieldSize.y - 1.0;
}

vec3 grassGroundNormal(vec2 xz) {
    float e = grass.field.z;
    float hx = grassFieldAt(xz + vec2(e, 0.0)).r - grassFieldAt(xz - vec2(e, 0.0)).r;
    float hz = grassFieldAt(xz + vec2(0.0, e)).r - grassFieldAt(xz - vec2(0.0, e)).r;
    return normalize(vec3(-hx, 2.0 * e, -hz));
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

// --- What grows where (shared with the turf, scene_ground.glsl) -------------

// Bunch grass: tussocks one a GRASS_TUSSOCK square, a cell's roots gathered
// toward its own (up to GRASS_TUSSOCK_GATHER of the way), bare ground left
// between them. The scene_mesh_drawer_grass.cpp tile bound allows for the
// furthest a root moves (kGrassTussockReach).
const float GRASS_TUSSOCK = 0.42;          // m
const float GRASS_TUSSOCK_GATHER = 0.55;
// How bunched the grass grows at xz: 0 a sward of small tufts .. 1
// tussocks; the node's `tufts` about a shared noise of a few metres.
float grassBunch(vec2 xz) {
    float n = sceneWindNoise(xz * 0.11 + vec2(23.1, -7.7)) * 0.5 + 0.5;
    return clamp(grass.cells.x * (0.3 + 1.4 * n), 0.0, 1.0);
}
// The tussock whose square holds xz: xy its centre (anywhere in the square,
// so they lie in no lattice), z how hard it gathers its roots (some loose,
// some tight).
vec3 grassTussock(vec2 xz) {
    vec2 g = floor(xz / GRASS_TUSSOCK);
    ivec2 gi = ivec2(g);
    vec2 o = vec2(sceneWindHash(gi + ivec2(311, 97)), sceneWindHash(gi + ivec2(-53, 211)));
    return vec3((g + 0.05 + 0.9 * o) * GRASS_TUSSOCK, 0.35 + 0.65 * sceneWindHash(gi + ivec2(19, -401)));
}
// Flowering culms a square metre at a `flowering` share of 1.
const float GRASS_CULMS = 150.0;
const float GRASS_NEAR_BLADES = 1400.0;   // the near ring's blades a square metre (grass_node.cpp's kRingSpec[0])
// Where the grass flowers and how much of it is dead at xz: x the share of
// tufts sending up a culm (in patches of tens of metres, more where it is
// lush and tall), y the share of leaves that are dead (more where dry).
vec2 grassKinds(vec2 xz, float lush, float tall) {
    float n = sceneWindNoise(xz * 0.07 + vec2(-41.3, 12.9)) * 0.5 + 0.5;
    float flower = grass.cells.y * 2.0 * smoothstep(0.2, 0.7, n) * (0.35 + 0.65 * lush * (0.6 + 0.4 * tall));
    float dead = min(grass.cells.z * (0.5 + 2.2 * (1.0 - lush)), 0.6);
    return vec2(min(flower, 1.0), dead);
}
// A dead leaf's colour: weathered straw to dun (t 0..1).
vec3 grassDeadColor(float t) { return mix(vec3(0.30, 0.255, 0.16), vec3(0.15, 0.13, 0.095), t); }
// A culm's head: fresh, green flushed with purple; ripe, straw (r 0..1).
vec3 grassHeadColor(float ripe) { return mix(vec3(0.105, 0.11, 0.065), vec3(0.40, 0.33, 0.18), ripe); }
// A whole head's mean colour once its spikelets are under a pixel: their
// green and purple with their pale anthers and the light on their glumes
// (scene_grass_surface.glsl's far heads; the turf's flowering).
vec3 grassHeadFarColor(float ripe) { return mix(vec3(0.25, 0.2, 0.19), vec3(0.42, 0.35, 0.21), ripe); }
// The sward as one surface, as the eye sees it from afar (a far blade and
// the turf where it stands for the blades). Its blades stand on average
// along `grassSwardUp`: the ground's normal `gn` tipped downwind by `lean`
// (the wind's comb and its gusts, across the ground), the way the bent
// blades lean. The direct light on it is its blades' mean as each is lit
// (scene_lighting.glsl's sceneSwardLight, SceneSurface::sward), so how a
// far meadow takes the sun follows from how its blades stand and where the
// eye and the sun are, as the near blades' own faces do: darker than level
// ground under a high sun seen across, faces lit with the sun behind the
// eye, shaded sides glowing through looking toward it; a gust's wave turns
// the blades it bends. What it mirrors of the sky and the sky's light on
// it take the faces' mean facet, the way to the eye bent up toward the
// blades' way (grassCanopyNormal; mesh.frag, scene_ground.glsl).
const float GRASS_CANOPY_UP = 0.5;   // mesh.frag's SCENE_GRASS (keep in step)
vec3 grassSwardUp(vec3 gn, vec2 lean) { return normalize(gn + vec3(lean.x, 0.0, lean.y)); }
vec3 grassCanopyNormal(vec3 up, vec3 toEye) { return normalize(toEye + GRASS_CANOPY_UP * up); }
// How much of the herbs' leaves and blooms past their plants shows among
// the grass (scene_herb_mix.glsl's herbMixMean counts them as if nothing
// stood round them; among the blades most leaves are hidden, as the
// plants' are, and some blooms): as much as the plants drawn up to their
// range show, so they hand over to the far blades and the turf unseen.
const float GRASS_HERB_LEAF_SEEN = 0.1;
const float GRASS_HERB_BLOOM_SEEN = 0.5;
// The lay of the sward seen from afar: how its blades lean across the
// ground past the wind's mean lean, as a tilt of grassSwardUp. Streaks
// combed along the wind (tens of metres long, a few across: the rest lay a
// steady wind leaves) and each gust `gust` travelling through it, bending
// the blades it crosses, so a far meadow shows the wind's sheen and its
// waves where single blades are too fine to see bend. A wind lays a sward
// only downwind, more in one streak than the next, never back against it;
// sceneSwardLight gathers the blades' leans that way by it (SWARD_COMB).
const float GRASS_COMB = 0.6;
const float GRASS_GUST_LEAN = 2.4;
// How far the wind's lay and its gusts show in the sward's sheen: none in
// still air (the gust field and the wind's way run on whatever the steady
// wind), all from half the reference wind.
float grassGustStir() { return clamp(2.0 * camera.windField.z / GRASS_REFERENCE_SPEED, 0.0, 1.0); }
vec2 grassCombLean(vec2 xz, float gust) {
    vec2 d = camera.windField.xy;
    float along = dot(xz, d), across = dot(xz, vec2(-d.y, d.x));
    float comb = 0.5 + 0.5 * (sceneWindNoise(vec2(along * 0.035, across * 0.3)) +
                              0.5 * sceneWindNoise(vec2(along * 0.09 + 7.1, across * 0.8 - 3.3)));
    float calm = clamp(camera.windField.z / GRASS_REFERENCE_SPEED, 0.0, 1.0);
    return d * (GRASS_COMB * comb * (0.4 + 0.6 * calm) + GRASS_GUST_LEAN * gust) * grassGustStir() * grass.blade.w;
}

// The share of a blade's shade along it that the eye sees on average from
// afar (the dark root and lit tip, weighted to the upper part that shows
// over the blades in front): a far blade takes this once along its length.
float grassMeanShade() { return mix(grass.look.x, 1.0, 0.62); }

// A culm's head runs from GRASS_CULM_HEAD up its card (the top fifth or so
// of the culm); the first row of the card ends its bare stem at
// GRASS_CULM_STEM.
const float GRASS_CULM_STEM = 0.7;
const float GRASS_CULM_HEAD = 0.76;
// A far ring's live leaf is a fan card (grassFanSurface): this many leaves,
// the card this many times the leaf's width at its middle, their widths
// together GRASS_FAN_COVER of it (the leaf's cover: they are shorter than
// the card).
const float GRASS_FAN_STRANDS = 4.0;   // scene_grass_surface.glsl's loop
const float GRASS_FAN_WIDTH = 2.2;
const float GRASS_FAN_COVER = 1.8;
// A far herb's bloom on a fan (grassFanBloom): its dot's share of what the
// fan's leaves cover.
const float GRASS_FAN_BLOOM_AREA = 0.8;
// A colour packed in one float (7 bits a channel, in sqrt, so the dark end
// keeps its steps): a varying the blade's vertices all share, so it reaches
// the fragment whole (scene_grass_surface.glsl's grassUnpackColor).
float grassPackColor(vec3 c) {
    vec3 q = floor(sqrt(clamp(c, 0.0, 1.0)) * 127.0 + 0.5);
    return q.r * 16384.0 + q.g * 128.0 + q.b;
}

// --- A blade -------------------------------------------------------------------

struct GrassBlade {
    vec3 pos;      // camera-relative
    vec3 normal;   // the blade's own
    vec3 side;     // across the blade
    vec3 ground;   // the ground's normal under it
    vec3 color;    // linear, before the node colour
    float ao;      // 0.5 at the root .. 1 at the tip
    float bulk;    // how much of its shading follows the ground's normal
    float far;     // 0 near .. 1: how far its translucency has faded
    vec2 uv;       // x: across, in the blade's base width (a culm's: its head's), 0 its middle; y: along, 0 .. 1
    // x: kind (0 a leaf, 1 a flowering culm, 2 a dead leaf, 3 a far fan of
    // leaves) plus under 0.5 a draw of its own; y: a culm's head, 0 a loose
    // panicle .. 15 a dense spike, plus the share of its card it truly
    // covers (a fan's: a leaf's width in the card's); z: its ripeness (a
    // fan's: 1 with a herb's bloom over it); w: its stem's width over its
    // head's in 1/1000s, plus the share of its card the stem truly covers
    // (a fan's: its bloom's colour, grassPackColor).
    vec4 info;
};

// The draw's rectangle of cells: its first column and row and its width in
// cells, in the model matrix's first row (scene_mesh_drawer_grass.cpp: a
// draw a run of the tiles in view).
vec3 grassRect() {
#ifdef GRASS_SHADOW
    return push.modelRows[0].xyz;
#else
    return vec3(push.model[0][0], push.model[1][0], push.model[2][0]);
#endif
}

// A hash of cell `c` and slot `k` in [0, 1): a blade's own draws.
float grassHash(ivec2 c, int k) {
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(k) * 2246822519u;
    h = (h ^ (h >> 15)) * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) / 4294967296.0;
}

// No blade bends further than this from upright (its tip some 40 degrees
// under level); a bend toward it saturates rather than stops.
const float GRASS_TILT_MAX = 2.3;
// The unit direction of a blade tilted `t` (rad from upright, toward t's
// way across the ground).
vec3 grassDir(vec2 t) {
    float m = length(t);
    float s = m > 1e-4 ? sin(m) / m : 1.0;
    return vec3(t.x * s, cos(m), t.y * s);
}
// A blade's tilt at `s` (0 root .. 1 tip) along it: its lean at the root
// `r`, the wind's and the movers' bend `w` (a leaf's stiff foot gives least,
// a culm bends evenly along), its rest curvature `c` (arching more toward
// the tip). All are tilt vectors, so they add without a flip whatever their
// ways, and the sum saturates at GRASS_TILT_MAX.
vec2 grassTilt(vec2 r, vec2 w, vec2 c, float s, bool culm) {
    float ws = culm ? s : s * (0.6 + 0.4 * s);
    vec2 t = r + w * ws + c * pow(s, 1.6);
    float m = length(t);
    return m > 1e-4 ? t * (GRASS_TILT_MAX * tanh(m / GRASS_TILT_MAX) / m) : t;
}

// Instance `id` of the draw at vertex `vid` of its ring mesh, blade-space
// vertex `v` (x across: -0.5, 0.5 or 0 at the tip; y 0 at the root .. 1 at
// the tip). False when the draw does not draw that blade.
bool grassBlade(int id, int vid, vec3 v, vec3 vn, out GrassBlade b) {
    b.pos = vec3(0.0);
    b.normal = vec3(0.0, 1.0, 0.0);
    b.side = vec3(1.0, 0.0, 0.0);
    b.ground = vec3(0.0, 1.0, 0.0);
    b.color = vec3(0.0);
    b.ao = 1.0;
    b.bulk = grass.lush.w;
    b.far = 0.0;
    b.uv = vec2(0.0);
    b.info = vec4(0.0);

    vec3 rect = grassRect();
    int W = max(int(rect.z), 1);
    ivec2 c = ivec2(int(rect.x) + id % W, int(rect.y) + id / W);
    vec4 h = vec4(sceneWindHash(c), sceneWindHash(c + ivec2(71, 13)), sceneWindHash(c + ivec2(-35, 29)),
                  sceneWindHash(c + ivec2(17, -91)));
    float cell = grass.cells.w;
    vec2 root = (vec2(c) + h.xy) * cell;
    // Bunch grass: the cell's root gathers toward its tussock.
    float bunch = grassBunch(root);
    vec3 tus = grassTussock(root);
    vec2 tc = tus.xy;
    root = mix(root, tc, GRASS_TUSSOCK_GATHER * tus.z * bunch);
    vec3 eye = camera.eyeWorld.xyz;
    // Across the ground and the eye's lift over it (lod.w): the rings scale
    // with the eye's height.
    float d = length(vec2(length(root - eye.xz), grass.lod.w));

    // Thinning, first since it is the cheapest test: past lod.x the instance
    // keeps fewer of its blades, widened to keep the cover; shadow casters
    // keep fewer again.
    int vpb = max(int(grass.shape.y + 0.5), 1);
    int j = vid / vpb;
    float keep = clamp(grass.lod.x / max(d, 1e-3), grass.lod.y, 1.0);
#ifdef GRASS_SHADOW
    keep *= grass.lod.z;
#endif
    float kept = max(grass.shape.x * keep, 1.0);
    if (float(j) + grassHash(c, 4096 + j) * 0.999 >= kept + 0.5) return false;

    // The ring, handing over to its neighbours through a dithered band (a
    // whole instance at a time).
    float fin = grass.ring.x <= 0.0 ? 1.0 : smoothstep(grass.ring.x - grass.ring.z, grass.ring.x, d);
    float fout = 1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, d);
    if (h.w > fin || h.w > fout) return false;
    if (!grassInField(root)) return false;
    vec4 f = grassFieldAt(root);

    // Off screen: a generous box around the cell's middle.
    float H = grass.blade.x;
    vec4 cp = camera.viewProj * vec4(root.x - eye.x, f.r + 0.6 * H - eye.y, root.y - eye.z, 1.0);
    float m = cp.w * 0.15 + 2.5 * H + 0.5 + grass.shape.z;
    if (cp.w < -2.0 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;

    if (f.g <= 0.0 || f.a <= 0.0) return false;

    // Clumps: thick and tall in some places, thin and short between. The
    // density thins blade by blade, so a thin patch is sparse tufts.
    vec3 gn = grassGroundNormal(root);
    float k = grass.dry.w;
    float clump = clamp((0.5 * sceneWindNoise(root * k) + 0.25 * sceneWindNoise(root * k * 2.03 + 17.3)) * 0.667 + 0.5,
                        0.0, 1.0);
    float minY = grass.blade.z;
    float dens = (0.55 + 0.7 * clump) * f.g * smoothstep(minY - 0.05, minY + 0.05, gn.y);
    vec4 hb = vec4(grassHash(c, j * 4), grassHash(c, j * 4 + 1), grassHash(c, j * 4 + 2), grassHash(c, j * 4 + 3));
    if (hb.w > dens) return false;
    vec4 he = vec4(grassHash(c, 2048 + j * 4), grassHash(c, 2049 + j * 4), grassHash(c, 2050 + j * 4),
                   grassHash(c, 2051 + j * 4));

    // What the blade is. Culms are the same share of the blades in every
    // ring (GRASS_CULMS a square metre at a share of 1 among the near ring's
    // GRASS_NEAR_BLADES), so their share of the cover holds as the rings
    // hand over: a far ring's culm stands for as many culms as its leaves
    // do leaves. Dead leaves are a share of the leaves.
    float lush = 1.0 - clamp(f.b, 0.0, 1.0);
    float tall = smoothstep(0.35, 0.8, sceneWindNoise(root * 0.045 + 3.0) * 0.5 + 0.5);
    vec2 kinds = grassKinds(root, lush, tall);
    float pc = kinds.x * GRASS_CULMS / GRASS_NEAR_BLADES;
    bool culm = he.x < pc;
    bool dead = !culm && he.x > 1.0 - kinds.y;
    float age = hb.z;

    // Its foot: tillers rise from a small crown, the young ones from its
    // middle (a tuft near the eye; further off blades over the cell).
    bool tufted = grass.shape.w > 0.0;
    float rr = sqrt(hb.x);
    float ra = hb.y * 6.2831853;
    vec2 out2 = vec2(cos(ra), sin(ra));
    float crown = tufted ? mix(0.45, 0.3, bunch) : 1.0;
    vec2 xz = root + out2 * (rr * mix(0.55, 1.0, age) * crown * grass.shape.z);
    float y0 = grassInField(xz) ? grassFieldAt(xz).r : f.r;
    // The way it leans: out from the crown (and from its tussock's middle,
    // a fountain), a little astray; at random over a far cell.
    float hr = fract(hb.x * 13.73 + hb.z * 5.17);
    vec2 away = out2 * rr + (root - tc) * (bunch * 3.0 / GRASS_TUSSOCK);
    float az = tufted && dot(away, away) > 1e-8 ? atan(away.y, away.x) + (he.z - 0.5) * 1.1 : hr * 6.2831853;
    vec2 lean = vec2(cos(az), sin(az));

    // Its length: the tuft's height (clumps, the field, tall lush patches)
    // times its own. Leaves grow longer with age, culms stand over them,
    // dead leaves are the short old ones at the foot.
    float ht = H * mix(0.5, 1.4, clump * clump) * (0.85 + 0.3 * h.z) *
               mix(1.0, mix(0.8, 1.7, tall * lush), grass.look.z) * f.a;
    float len = culm ? ht * mix(1.35, 1.75, he.y)
              : dead ? ht * mix(0.25, 0.7, he.y)
                     : ht * mix(0.45, 1.3, age) * (0.85 + 0.3 * he.y);

    // Its rest posture: a lean at the root and a curvature along it. Young
    // leaves stand, older ones lean out and arch, the long ones drooping
    // past level under their own weight (outer leaves of a tussock most);
    // a few lie lodged; the dead lie at the foot; a culm stands, its head
    // nodding a little.
    float r0, droop;
    if (culm) {
        r0 = 0.04 + 0.12 * he.z;
        droop = 0.1 + 0.25 * he.w;
    } else if (dead) {
        r0 = 0.75 + 0.55 * he.z;
        droop = 0.5 + 0.7 * he.w;
    } else {
        // Sun-dried leaves have lost their turgor: they splay and flop.
        float wilt = 1.0 - lush;
        r0 = mix(0.1, 0.75, age) + 0.3 * wilt * age;
        droop = (0.25 + 1.5 * age * sqrt(age)) * clamp(len / 0.4, 0.3, 2.0) * mix(0.75, 1.25, he.w) * (1.0 + 0.5 * bunch * rr) *
                (1.0 + 0.8 * wilt);
        if (he.w < 0.04) {
            r0 = 1.1 + 0.3 * he.y;
            droop *= 0.4;
        }
    }

    // The wind: a lean downwind, the travelling gusts, and each blade's own
    // sway along its lean, each by the blade's flex (a long leaf gives most,
    // a culm's head catches it, a dead leaf hardly stirs); then the movers'
    // push, and their trample as a lean from the root the way it was pushed.
    float gu = sceneWindGustAt(root);
    float calm = clamp(camera.windField.z / GRASS_REFERENCE_SPEED, 0.0, 2.0);
    float spd = camera.windField.z + gu * 4.0;
    vec2 wdir = camera.windField.xy;
    float flex = culm ? 1.2 : dead ? 0.35 : mix(0.7, 1.3, he.w) * clamp(0.55 + 0.6 * len / max(H, 1e-3), 0.6, 1.5);
    float flut = sin(6.2831853 * sceneWindCycles((2.2 + hb.y * 2.5) / 6.2831853) + dot(xz, wdir) * 0.9 + hr * 9.0) *
                 (0.06 * calm + 0.12 * gu);
    vec2 Wb = (wdir * (0.1 * min(calm, 1.0) + spd * 0.07 + gu * 0.45) + lean * flut) * (grass.blade.w * flex);
    float trample;
    vec2 lie;
    vec2 pb = grassInfluence(xz, trample, lie);
    trample = min(trample, 1.0);
    // A culm's stem is stiffer than a leaf and carries its head high, so a
    // push swings it less and it is seldom laid flat.
    Wb += pb * (culm ? 0.7 : 1.6);
    float flattened = trample * mix(0.75, 1.0, hb.z) * (culm ? 0.5 : 1.0);
    vec2 R = lean * r0 + lie * (flattened * 1.3 / max(length(lie), 0.25));
    vec2 C = lean * droop;

    // Along it: a culm's card spends its first row on the bare stem and the
    // rest on its head.
    int seg = max((vpb - 1) / 2, 1);
    float k1 = 1.0 / float(seg);
    float y = v.y;
    if (culm) y = y <= k1 + 1e-4 ? y / k1 * GRASS_CULM_STEM : GRASS_CULM_STEM + (y - k1) / (1.0 - k1) * (1.0 - GRASS_CULM_STEM);
    // Its point there: its direction integrated from the root (Simpson's
    // rule over the tilt at the root, halfway and here).
    vec2 T0 = grassTilt(R, Wb, C, 0.0, culm);
    vec2 Th = grassTilt(R, Wb, C, 0.5 * y, culm);
    vec2 Ty = grassTilt(R, Wb, C, y, culm);
    vec3 tang = grassDir(Ty);
    vec3 P = (grassDir(T0) + 4.0 * grassDir(Th) + tang) * (y * len / 6.0);
    P.y = max(P.y, 0.01 * len * y);   // a lodged leaf lies on the ground, not through it
    vec3 base = vec3(xz.x - eye.x, y0 - eye.y, xz.y - eye.z);

    // Its face: square to its lean (a leaf arches over its flat), turned a
    // little astray and twisting along its length. A culm turns to the eye.
    float twist = (he.z - 0.5) * 1.6;
    float yaw = az + 1.5707963 + (fract(he.y * 7.7) - 0.5) * 0.5 + twist * y;
    vec3 side = vec3(cos(yaw), 0.0, sin(yaw));
    if (culm) {
        vec3 s2 = cross(tang, -(base + P));
        if (dot(s2, s2) > 1e-8 * dot(base + P, base + P)) side = s2;
    }
    side -= tang * dot(side, tang);
    float sl = dot(side, side);
    side = sl > 1e-8 ? side * inversesqrt(sl) : normalize(vec3(-tang.y, tang.x, 0.0) + vec3(0.0, 0.0, 1e-3));

    // Its width: growing with distance, and never under about two thirds
    // of a pixel (a thinner blade only flickers in and out of the samples).
    // A leaf tapers to a fine point over its length; one only a pixel or so
    // wide keeps a blunter taper, so its tip does not flicker either.
    float px = 2.0 / max(abs(camera.proj[1][1]) * camera.viewport.y, 1.0);
    float dv = length(vec3(xz.x, y0, xz.y) - eye);
    float wdRaw = grass.blade.y * (1.0 + d * grass.look.w) * (grass.shape.x / kept);
    float wd = max(wdRaw, dv * px * 0.65);
    float across = v.x > 1e-4 ? 0.5 : v.x < -1e-4 ? -0.5 : 0.0;
    float wv, ref;
    bool fan = !tufted && !culm && !dead;
    if (culm) {
        // The stem a third of a leaf's width; a loose panicle spreads some
        // eight leaf widths, a dense spike three (of the ring's leaves, as a
        // far culm stands for as many culms as its leaves do leaves). The
        // fragment cuts both from the card, which is at least about a pixel
        // across; what that covers past their true widths (kHead, kStem) the
        // fragment takes back.
        float form = smoothstep(0.35, 0.65, sceneWindNoise(root * 0.05 + vec2(91.0, -13.0)) * 0.5 + 0.5);
        float stemP = wdRaw * 0.3, headP = wdRaw * mix(8.0, 3.0, form);
        float stemW = max(stemP, dv * px * 0.7);
        float headW = max(headP, dv * px * 1.2);
        bool tip = v.y > 1.0 - 1e-4;
        wv = tip ? 0.0 : (v.y < 1e-4 || (seg > 2 && y < GRASS_CULM_STEM + 1e-3)) ? stemW : headW;
        ref = headW;
        // Packed: whole parts the stem's share of the card (1/1000s) and the
        // head's form (1/15s), fractions what each covers of its card.
        b.info.w = floor(stemW / headW * 1000.0 + 0.5) + 0.99 * (stemP / stemW);
        b.info.y = floor(form * 15.0 + 0.5) + 0.99 * (headP / headW);
        b.info.z = clamp(0.9 - lush + 0.4 * (he.z - 0.5), 0.0, 1.0);
    } else if (fan) {
        // A far live leaf stands for a few: a card fanning out from a narrow
        // foot (GRASS_FAN_WIDTH of its width at the middle), cut per pixel
        // into GRASS_FAN_STRANDS tapering leaves (scene_grass_surface.glsl's
        // grassFanSurface), so a far sward reads as fine blades, not broad
        // leaves. Their width keeps the leaf's cover (a strand is shorter
        // than the card and tapers).
        float Wm = wd * GRASS_FAN_WIDTH;
        wv = Wm * mix(0.35, 1.0, clamp(2.0 * y, 0.0, 1.0));
        ref = Wm;
        b.info.y = GRASS_FAN_COVER / GRASS_FAN_STRANDS / GRASS_FAN_WIDTH * mix(0.75, 1.35, age);
    } else {
        float sharp = smoothstep(0.9, 2.5, wdRaw / max(dv * px, 1e-6));
        float taper = pow(max(1.0 - pow(y, 2.2), 0.0), 0.8) * (0.8 + 0.2 * smoothstep(0.0, 0.2, y));
        // An older leaf is broader (a young one a narrow rolled shoot).
        wv = wd * mix(pow(max(1.0 - y, 0.0), 0.7), taper, sharp) * (dead ? 0.8 : mix(0.75, 1.35, age));
        ref = wd;
    }
    b.info.x = (culm ? 1.0 : dead ? 2.0 : fan ? 3.0 : 0.0) + 0.49 * fract(he.w * 17.3 + hb.x);
    b.pos = base + side * (across * wv) + P;
    b.normal = normalize(cross(side, tang));
    b.side = side;
    b.ground = gn;
    b.uv = vec2(across * wv / ref, y);

    // Colour: lush to dry by the mask and the clump, each blade its own value
    // and a little hue (the streaks that make a sward read as blades rather
    // than as a painted surface); a leaf's sheath paler and yellower at its
    // foot; dead leaves straw to dun; a culm's stem greener than its head.
    // An outer ring's blade is fewer, wider blades' worth: its value spreads
    // a little wider, so the sward's grain keeps its contrast as the rings
    // hand over.
    float coarse = pow(max(grass.blade.y / max(grass.field.w, 1e-6), 1.0), 0.12);
    // Once a blade is a pixel or two wide its own value is noise: many
    // blades share a pixel and theirs average out. What still shows is how
    // the blades of a tussock differ from the next one's (its crown in
    // more light, a hollow between, its own age and greenness), so a far
    // blade takes its tussock's value and hue more than its own.
    float own = mix(1.0, clamp(wdRaw / (dv * px * 12.0), 0.15, 1.0), smoothstep(10.0, 40.0, d));
    ivec2 tg = ivec2(floor(root / GRASS_TUSSOCK));
    float tusTone = sceneWindHash(tg + ivec2(-71, 503)), tusHue = sceneWindHash(tg + ivec2(907, -33));
    float tone = 0.975 + mix(tusTone - 0.5, fract(hb.y * 7.31 + hb.x) - 0.5, own) * 0.55 * coarse;
    float hue = mix(tusHue, fract(hb.w * 5.31 + hr), own);
    vec3 green = grass.lush.rgb * mix(vec3(0.85, 0.95, 1.1), vec3(1.15, 1.05, 0.8), hue) * tone;
    vec3 dryc = grass.dry.rgb * mix(0.85, 1.15, hr) * tone;
    vec3 col = mix(dryc, green, clamp(lush * 1.3 - 0.15 + clump * 0.2, 0.0, 1.0));
    if (dead) col = mix(grassDeadColor(he.y), dryc, 0.3) * mix(0.85, 1.15, hr);
    else if (culm) col = mix(green * vec3(1.05, 1.1, 0.9), dryc, 0.2 + 0.5 * (1.0 - lush));
    else col = mix(col * vec3(1.3, 1.25, 0.75) + vec3(0.01, 0.012, 0.0), col * mix(1.08, 0.94, age), smoothstep(0.03, 0.15, y));
    // Shade: dark low in the sward (its depth by how high the point stands,
    // so a drooping tip low down is shaded too). Far off a blade is a pixel
    // or two: its dark root would leave only the tips showing (a field of
    // dots), so with distance the blade takes the mean of that shade along
    // its whole length and reads as the sward it stands for, as dark as the
    // near blades are on average.
    float sh = clamp(max(P.y / max(0.7 * ht, 1e-3), 0.5 * y), 0.0, 1.0);
    float far = smoothstep(12.0, 70.0, d) * 0.75;
    col *= mix(mix(grass.look.x, 1.0 + 0.2 * sh, sh), grassMeanShade(), far);
    col *= 1.0 + gu * grass.look.y * sh * 1.2;
    col *= 1.0 + 0.4 * flattened;   // a flattened lane shows the blades' paler length
    // Past the herbs' plants a far fan stands for the herbs among the grass
    // as often as their cover seen from here has them (herbMixMean; the
    // turf, between the fans and past them, does the same): their leaves'
    // mean colour, or one of their blooms by the colonies there
    // (herbBloomLocal), so the flowers the plants drew up to their range go
    // on as specks of their colour, patch by patch.
#ifndef GRASS_SHADOW
    if (fan && grass.herbInfo.x > 0.5) {
        float tanView = grass.lod.w / max(length(root - eye.xz), 1e-3);
        float stand = 1.0 - herbPlantsDrawn(d, tanView);
        float u = grassHash(c, 6144 + j);
        if (u < stand) {
            vec2 fh = grassFloorHerbsAt(root);
            HerbMix hm = herbMixMean(fh.x, fh.y, tanView);
            if (fh.y > 0.0 && u < stand * hm.cover) {
                vec3 bloom, pick;
                float u2 = fract(u * 61.7 + hb.x);
                float more = herbBloomLocal(root, fh.x, fract(u2 * 37.3), bloom, pick);
                float share = min(hm.bloomShare * more, 0.95);
                // A bloom is a dot over the fan's top (scene_grass_surface.glsl's
                // grassFanBloom), not the whole fan: a share of it about
                // GRASS_FAN_BLOOM_AREA, so as many more fans carry one.
                float blooms = min(share * GRASS_HERB_BLOOM_SEEN / GRASS_FAN_BLOOM_AREA, 0.9);
                if (u2 < blooms) b.info.zw = vec2(1.0, grassPackColor(pick));
                else if (u2 < blooms + (1.0 - hm.bloomShare) * GRASS_HERB_LEAF_SEEN) col = hm.leaf;
            }
        }
    }
#endif
    b.color = col;
    b.ao = mix(mix(0.5, 1.0, sh), 0.8, far);
    // Near the eye a blade is lit as its own face; further off more and more
    // as the sward it stands in (a hillside reads as turf, not a field of
    // bright tips): its blades' mean as the eye sees them (mesh.frag's
    // SCENE_GRASS turns this way into the sward's light and facet).
    b.bulk = mix(mix(0.3, grass.lush.w, smoothstep(2.0, 14.0, d)), 0.95, far);
    // The way its blades stand: the ground's normal tipped by the sward's
    // lay and its gusts (grassCombLean), from where the sward's light takes
    // over from the blade's own face (mesh.frag's SCENE_GRASS, 2-12 m). The
    // lay is the sward's, steady while the blade stirs.
    vec2 lay = grassCombLean(root, gu) * smoothstep(2.0, 12.0, d);
    b.ground = grassSwardUp(gn, lay);
    // Seen against a low sun a far meadow still glows through its blades,
    // less than a near blade held to the light.
    b.far = smoothstep(8.0, 40.0, d) * 0.25;
    return true;
}
