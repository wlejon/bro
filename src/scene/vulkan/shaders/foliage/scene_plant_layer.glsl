// What stands as a plant: the trees and shrubs a plants node draws, as a
// world writes them (docs/scene-world-architecture.md §13, "worlds as
// code"). Included by scene_plants.glsl; needs scene_camera.glsl and
// scene_plant_records.glsl.
//
// What a plant is is not the engine's: a world gives the plants node a
// MODULE, GLSL compiled into the plants' shaders at run time (PlantsNode::
// setModule; foliage/scene_plant_shaders.h). It defines the functions
// declared at the end of this file, and may use anything of
// scene_camera.glsl (the wind) and scene_plant_records.glsl (the plant, the
// engine's share of its species, the crown's envelope, the hashes). Its
// data is its share of each species' record, plantData(species, i).
//
// The engine owns everything around the module's answers: where the plants
// stand (the rules, the cells, streaming), how many elements each is drawn
// with in each view (the plan: an element kept about `pixels` across, a
// prefix of the one sequence, a far caster a disc), the leaf polygon and
// the tube they are drawn as, their bounds, and the light (the crown as a
// medium, its own shade toward the sun, the shadows, the probes).
//
// The module's functions:
//   plantElement   (PLANT_BUILD) element k of a plant drawn n strong:
//                  where its polygon lies, its normals and colour. Every
//                  prefix of the sequence should cover the whole crown, each
//                  element growing as n falls (PlantElementIn.grow), so a far
//                  plant is a few broad daubs.
//   plantLeafPoint the element polygon's point c (base, four pairs up its
//                  sides, tip) as (across, along), its outline at a detail
//                  (0 coarse .. 1 a near leaf's blade; PLANT_LEAF_POINTS).
//   plantLimb      tube `limb` of a plant (0 its trunk, then its limbs): its
//                  ends, radii and colour.
//   plantSway      the wind's push on a point of a plant.
//   plantSurface   (PLANT_SHADE) what a fragment of an element, a tube or a
//                  painted crown is: its colour, the normal it is lit by,
//                  the light through it.

#define PLANT_MAX_LIMBS 48   // PP_MAX_LIMBS

// What the engine asks of element k (plantElement's).
struct PlantElementIn {
    int k;          // its place in the sequence
    float n;        // the elements the plant is drawn with (its fraction grows the last in)
    bool caster;    // a shadow view's: fewer, larger, never the eye's
    float grow;     // sqrt(full / n): how much larger each is for being fewer
    float last;     // 1, or n's fraction for the last element (scale it by this)
};

// An element: its polygon (base its (0, 0) point, along to its tip, across
// its width; camera-relative), and how it is lit.
struct PlantElement {
    vec3 base;
    vec3 along;
    vec3 across;
    vec3 leafNormal;    // world: its own face
    vec3 crownNormal;   // world: the form it is lit as
    vec3 albedo;        // linear
    vec3 crownCenter;   // camera-relative: the envelope's centre as the element's sway moved it
    float far;          // 0 a leaf .. 1 a daub standing for many
    float depth;        // 0 its lobe's skin .. 1 its core: how much of the sky reaches it
};

// A tube: from `a` to `b` in the plant's frame (y up, metres, its foot at
// the origin), radius ra at a and rb at b.
struct PlantLimb {
    vec3 a;
    vec3 b;
    float ra;
    float rb;
    float lag;          // its far end's sway lag (plantSway's)
    vec3 albedo;
};

// A fragment, as plantSurface gets it.
struct PlantFragment {
    vec3 pos;           // camera-relative
    vec3 normal;        // its own face, toward the eye's side
    vec3 crownNormal;
    vec3 albedo;        // as built
    vec2 uv;            // an element's (plantLeafPoint's); a tube's 0; a painted crown's point on its disc
    float far;
    float depth;
    bool wood;
    int species;
    // A painted crown (past the scene's real band: the whole plant one disc
    // facing the eye, lit as a dome; far 1): uv its point across the disc
    // seen from the eye in the crown's horizontal radii, (0, 0) its middle
    // (about 1 at its rim), x to the eye's right and y up. What it looks like within the disc (its clumps,
    // their shade) is the module's.
    bool painted;
};
// (A painted crown's dome: its rim's normal this much outward of the eye's.)
#define PLANT_PAINT_RIM 0.75

// What a fragment is. The engine lights it: the sun through the crown's
// envelope (its density), wrapped about `normal`; `glow` the light the sun
// shows through it from behind (rgb, times the albedo); the sky by depth.
struct PlantSurface {
    vec3 albedo;
    vec3 normal;        // world, normalised
    vec3 glow;
};

// The module.
#ifdef PLANT_BUILD
bool plantElement(Plant p, PlantSpecies s, PlantCrown c, PlantElementIn k, inout PlantElement e);
#endif
// The element polygon's points: base 0, four pairs up its sides (left odd,
// right even) and tip 9. At detail 0 (a daub, a leaf of 4 px or less, every
// shadow view's) only 0, 1 2, 5 6 and 9 are drawn, so 3 4 and 7 8 must lie
// on its sides there; at 1 (from 12 px) the whole outline is drawn, and the
// module morphs between continuously.
#define PLANT_LEAF_POINTS 10
vec2 plantLeafPoint(int species, int c, float detail);
bool plantLimb(Plant p, PlantSpecies s, PlantCrown c, int limb, out PlantLimb l);
vec3 plantSway(Plant p, PlantSpecies s, float up, float lag);
#ifdef PLANT_SHADE
PlantSurface plantSurface(PlantFragment f);
#endif
