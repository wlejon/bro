// The herb layer: the low plants a grass node grows among its blades, as a
// world writes them (docs/scene-world-architecture.md §13, "worlds as
// code"). Included by scene_grass.glsl; needs scene_camera.glsl.
//
// What the plants are is not the engine's: a world gives the grass node a
// MODULE, GLSL compiled into the cover's shaders at run time (GrassNode::
// setHerbs; scene_cover_shaders.h). It defines the functions declared at the
// end of this file, and may use anything of scene_camera.glsl and
// scene_grass.glsl (and, under HERB_BUILD, scene_vegetation.glsl): its own
// structs, constants, noise and maths are its business. Its data, if it has
// any, is herbData(i) (the node's, up to GRASS_HERB_DATA vec4s).
//
// The engine owns everything around the module's answers: a grid of cells
// (grass.herbInfo.y m), each holding at most one plant; the rings it is
// drawn in (whole plants near, a few of their cards further, thinned and
// widened to keep their cover), their dithered hand-overs; the far tier's
// element past them (scene_cover_far.glsl) and the turf past that, which
// take the module's far look; the field and screen tests, the plan, the
// lighting, the dither of a part-covered pixel and its sub-pixel mark.
//
// The module's functions:
//   herbStand   which of its plants stands in a cell, if any: a variety
//               0..HERB_VARIETIES-1, or -1. Read by the plan (cover_plan.comp)
//               and by the far tier, at the cell's every use the same.
//   herbBuild   (HERB_BUILD) a vertex of one of the plant's cards: the ring
//               draws `cards` cards of `verts` vertices an instance (a strip,
//               x across -0.5..0.5, y along 0..1); a card the module does
//               not use is left out (false).
//   herbShade   (HERB_SHADE) what a fragment of a card is: its cover of the
//               pixel (cut per pixel, its edges antialiased), its colour, its
//               normal, its light.
//   herbFarMean the layer's look from afar over a stretch of ground, by the
//               canopy and density there and the view's elevation: what the
//               turf and the far blades stand in with.
//   herbFarLocal its blooms there, patch by patch, over that mean.
//   herbFarLook a variety as one element of the far tier: a mound of its
//               leaves dotted with its blooms.

#define GRASS_HERB_DATA 256      // GrassNode::kHerbDataVec4s
#define HERB_VARIETIES 15        // the far tier keeps a variety in 4 bits (coverPack), 0 a tuft

// The module's data (GrassNode::setHerbs).
vec4 herbData(int i) { return grass.herbData[i]; }

// A plant the plan listed, as its build gets it.
struct HerbPlant {
    ivec2 cell;        // its cell of the herbs' grid
    vec4 h;            // the cell's four hashes (grassHash(cell, 101..104)); h.xy placed its root in the cell
    vec2 rootXZ;       // absolute x, z
    vec3 root;         // camera-relative, on the ground
    vec3 ground;       // the ground's normal there
    vec2 floorAt;      // the floor there: the canopy (0 open .. 1 closed), the herbs' density
    float d;           // its distance from the eye (with the eye's lift)
    float farWiden;    // the far ring's widening of what it keeps (1 nearer): widen to keep the cover
    int variety;       // herbStand's
};

// A vertex of a card (mesh_herb.vert passes it on to mesh.frag's SCENE_HERB
// build). The engine reads the first part; `id`, `a`, `b`, `c` are the
// module's, handed back to its herbShade as they are (`id` flat, the rest
// interpolated).
struct HerbVertex {
    vec3 pos;       // camera-relative
    vec3 normal;    // the card's face
    vec3 side;      // across the card
    vec3 ground;    // the normal the plant's bulk is lit toward
    vec3 tint;      // the plant's own tone
    vec2 uv;
    float ao;
    float bulk;     // how much it is lit as the plant's bulk rather than its own face
    float seed;     // 0..1: its card's dither
    vec3 id;
    vec4 a;
    vec4 b;
    vec2 c;
};

// A fragment of a card, as herbShade gets it.
struct HerbFragment {
    vec3 pos;       // camera-relative
    vec3 normal;    // the card's face, as interpolated (not normalised)
    vec3 side;      // across (HerbVertex.side), interpolated
    vec3 along;     // cross(normal, side), interpolated
    vec2 uv;
    vec3 tint;
    vec4 bulk;      // xyz: HerbVertex.ground * bulk; w: -ao
    vec3 id;
    vec4 a;
    vec4 b;
    vec2 c;
};

// What a fragment is. The engine keeps `cover` as that share of the pixel's
// samples (its own dither, the card's seed), marks it for the sub-pixel
// resolve by its size on screen, and lights the rest. A card seen from
// behind is paled as a leaf's underside is.
struct HerbShade {
    float cover;          // 0..1
    vec3 albedo;
    vec3 normal;          // world space, normalised, toward the eye
    float roughness;
    float ao;             // the sky's share that reaches it
    float sun;            // the sun's share that reaches it (its stand's own shade)
    float translucency;   // >= 0 as is; < 0 the leaf's (the node's subsurface) times -it
    vec4 sward;           // lit as the sward's mean facet: xyz its up, w how much (0: its own face)
};

// The layer's look from afar (herbFarMean): the share of the ground it
// covers seen from the eye, its leaves' and blooms' mean colours and the
// blooms' share of what is seen.
struct HerbMix {
    float cover;
    vec3 leaf;
    vec3 bloom;
    float bloomShare;
};

// A variety as an element of the far tier: its height and its width (m;
// the engine widens it to at least a cell) at its own `size`, by which the
// engine scales both; its leaves' colour, its blooms' and their share of
// its face.
struct HerbFarLook {
    float height;
    float width;
    float size;
    vec3 body;
    vec3 bloom;
    float bloomShare;
};

// The module. `xz` absolute; `fp` a pixel's footprint (m; 0 none): a module
// fades detail finer than it to its mean. herbFarLocal's `u` (-1: none)
// draws one bloom's colour into `pick`; it returns the local blooms over
// their mean (0 none, 1 the mean), their colour in `bloom`.
int herbStand(ivec2 cell, vec2 xz, vec2 floorAt);
HerbMix herbFarMean(float canopy, float density, float tanView);
float herbFarLocal(vec2 xz, float canopy, float u, float fp, out vec3 bloom, out vec3 pick);
HerbFarLook herbFarLook(int variety, vec4 h);
#ifdef HERB_BUILD
bool herbBuild(HerbPlant p, int card, int cards, int verts, vec3 v, inout HerbVertex o);
#endif
#ifdef HERB_SHADE
HerbShade herbShade(HerbFragment f);
#endif

// --- The engine's hand-overs --------------------------------------------------

// The share of the far plants drawn at ring distance `d` seen at an
// elevation whose tangent is `tanView` (the eye's lift over the distance
// across the ground, as the rings measure it): seen from well above, a far
// plant is a dot the turf's cover from above stands for as well, so they
// are left to it (a view down over the land from a height draws none).
float herbFarSteep(float d, float tanView) {
    float far = smoothstep(grass.herbShape.w, grass.herbShape.w * 1.4, d);
    return 1.0 - far * smoothstep(0.25, 0.5, tanView);
}

// How much of the herbs the plants draw at ring distance `d` (1 inside the
// far plants' range, falling through their hand-over band and as the view
// steepens past the whole plants): the turf stands in for the rest.
float herbPlantsDrawn(float d, float tanView) {
    if (grass.herbInfo.x < 0.5) return 0.0;
    return (1.0 - smoothstep(grass.herbInfo.z - grass.herbInfo.w, grass.herbInfo.z, d)) * herbFarSteep(d, tanView);
}
