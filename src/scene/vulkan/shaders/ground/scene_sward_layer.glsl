// The sward: the blades a grass node grows over its land, as a world writes
// them (docs/scene-world-architecture.md §13, "worlds as code"). Included by
// scene_grass.glsl; needs scene_camera.glsl.
//
// What the blades are is not the engine's: a world gives the grass node a
// MODULE, GLSL compiled into the cover's shaders at run time (GrassNode::
// setSward; scene_cover_shaders.h). It defines the functions declared at the
// end of this file, and may use anything of scene_camera.glsl and
// scene_grass.glsl (the land's cover, the movers' push, the hashes): its own
// structs, constants, noise and maths are its business. Its data, if it has
// any, is swardData(i) (the node's, up to GRASS_SWARD_DATA vec4s).
//
// The engine owns everything around the module's answers: the rings of
// cells around the eye (finer near, coarser further), each cell's candidate
// blades, the thinning that keeps fewer of them with distance (asking the
// module for wider ones, so the cover holds), the dithered hand-overs
// between rings, the land's edge and the screen test, the plan; the far
// tier's tufts past the rings and the turf past those, which take the
// module's far look; the herbs' blooms carried by far blades; the lighting,
// the dither of a part-covered pixel and its sub-pixel mark.
//
// The module's functions:
//   swardRoot    where a cell's blades rise (it may gather them toward a
//                tussock), and anything of its own the cell's blades share.
//   swardDensity the chance a candidate stands there (a cell's blade, a far
//                tuft): its clumps, its slope limit, the land's density.
//   swardBuild   (SWARD_BUILD) a vertex of one of the cell's blades, at the
//                width the ring asks for: its kind, shape, bend, colour.
//   swardShade   (SWARD_SHADE) what a fragment of a blade is: its cover of
//                the pixel, its colour, its normal, its light.
//   swardFarLook a tuft of the far tier: its height, width and colour.
//   swardBake    the turf's surface layer's three channels of the sward's
//                patterns, band-limited to a footprint.
//   swardTurf    the sward as one surface, where the turf stands in for it:
//                its tops' and its roots' colours, its cover.
//   swardLay     the way its blades stand there (the turf is lit as them
//                where it stands for them), and a gust's lift of its sheen.

#define GRASS_SWARD_DATA 64   // GrassNode::kSwardDataVec4s

// The module's data (GrassNode::setSward).
vec4 swardData(int i) { return grass.swardData[i]; }

// A cell of a ring's grid, as the engine tests it and the module fills it.
struct SwardCell {
    ivec2 cell;    // its cell of the ring's grid (cells.w m a side)
    vec4 h;        // the cell's four hashes (h.xy placed its spot in the cell)
    vec2 root;     // absolute x, z: the hashed spot, then where swardRoot puts its blades' foot
    vec4 f;        // the land at the root (ground height, and its cover's density, dryness, height multiplier: grassGround)
    vec3 ground;   // the ground's normal there
    float d;       // its distance from the eye (with the eye's lift, as the rings measure it)
    float kept;    // the blades of the cell the ring keeps there (its thinning)
    vec4 own;      // the module's: swardRoot and swardDensity fill it, the rest read it
};

// How the ring draws a cell's blades (swardBuild's).
struct SwardRing {
    bool tufted;   // a near ring: a tuft rising from a crown (else blades over the cell)
    float spread;  // how far a cell's blades stand from its root (m)
    float width;   // the width the ring asks a blade for here (m): grown with distance, widened by its thinning
    float pixel;   // metres a pixel spans a metre from the eye (a blade's under about 0.65 of one only flickers)
    float coarse;  // the ring's blade over the node's finest: how many blades one stands for across
    int verts;     // vertices a blade: (verts - 1) / 2 segments, a strip up to a point
};

// A vertex of a blade (mesh_grass.vert passes it on to mesh.frag's
// SCENE_GRASS build). The engine reads the first part; `info` is the
// module's, handed back to its swardShade as it is.
struct SwardVertex {
    vec3 pos;       // camera-relative
    vec3 normal;    // the blade's own
    vec3 side;      // across the blade
    vec3 up;        // the way the sward's blades stand where it is lit as the sward
    vec3 color;     // linear, before the node colour
    float ao;       // the sky's share that reaches it (0.5 at the root .. 1 at the tip)
    float bulk;     // how much of its shading follows `up` rather than its own face
    float far;      // 0 near .. 1: how far its translucency has faded
    vec2 uv;        // x across its card (-0.5..0.5 its card's width), y along (0 root .. 1 tip)
    vec4 info;      // the module's
    bool carries;   // it may carry a far herb's bloom (a far blade standing for the sward's mean)
    float speck;    // the engine's: the herb bloom it carries (grassPackColor, 0 none)
};

// A fragment of a blade, as swardShade gets it.
struct SwardFragment {
    vec3 pos;         // camera-relative
    vec3 normal;      // the blade's face toward the eye's side (not normalised)
    vec3 side;        // across (SwardVertex.side), interpolated
    vec2 uv;
    vec4 info;
    vec3 color;       // SwardVertex.color times the node's colour
    vec3 bulk;        // SwardVertex.up times its bulk
    float fade;       // 0..1: how far its translucency has faded with distance (SwardVertex.far)
    float roughness;  // the node's
};

// What a fragment is. The engine keeps `cover` as that share of the pixel's
// samples (its own dither, by `seed`), marks it for the sub-pixel resolve
// by `px`, and lights the rest.
struct SwardShade {
    float cover;          // 0..1
    float seed;           // 0..1: its dither
    float px;             // its finest cut part's width in pixels (1e4: no cut part)
    vec3 albedo;
    vec3 normal;          // world space, normalised, toward the eye
    float roughness;
    float translucency;   // >= 0 as is; < 0 a leaf's (the node's subsurface, faded) times -it
    vec4 sward;           // lit as the sward's mean facet: xyz its up, w how much (0: its own face)
};

// A tuft of the far tier (swardFarLook): its height and width (m; the
// engine widens it to keep the cover it stands for) and its colour, shaded
// as the sward is on average.
struct SwardFarLook {
    float height;
    float width;
    vec3 body;
};

// What the turf asks of the sward at a point (swardTurf).
struct SwardTurfIn {
    vec2 xz;          // absolute
    vec3 rel;         // camera-relative
    float fp;         // a pixel's footprint (m): detail finer fades to its mean
    float density;    // the field's (or the ground mesh's) density, dryness
    float dry;
    vec3 baked;       // swardBake's channels here
    float d;          // ring distance
    float deep;       // 0 near .. 1: a gap among the blades is more sward, not the ground
    float nearSeen;   // 0..1: how much the ground between near blades shows its own detail
    vec3 soil;        // the bare ground's colour
};

// The sward as one surface: what the turf stands in with.
struct SwardTurf {
    vec3 tops;        // its blades' tops seen from afar
    vec3 roots;       // what is seen between them (their roots, the soil; deeper sward far off)
    float cover;      // the share of the ground its blades cover
};

// The module.
void swardRoot(inout SwardCell s);
float swardDensity(inout SwardCell s);
SwardFarLook swardFarLook(SwardCell s, vec4 h);
vec3 swardBake(vec2 xz, float fp);
SwardTurf swardTurf(SwardTurfIn t);
vec4 swardLay(vec2 xz, vec3 gn);
#ifdef SWARD_BUILD
bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout SwardVertex o);
#endif
#ifdef SWARD_SHADE
SwardShade swardShade(SwardFragment f);
#endif

// --- The engine's share of how a sward is lit -------------------------------

// The faces' mean facet of a sward whose blades stand along `up`, seen from
// `toEye`: what its sky light takes and what it mirrors (the blades' faces
// turned to the eye, bent up toward their way). mesh.frag's grass and the
// turf both light a far sward through it.
const float GRASS_CANOPY_UP = 0.5;
vec3 grassCanopyNormal(vec3 up, vec3 toEye) { return normalize(toEye + GRASS_CANOPY_UP * up); }
