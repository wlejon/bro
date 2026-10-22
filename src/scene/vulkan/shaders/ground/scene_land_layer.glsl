// The land's module contract (docs/scene-world-architecture.md §13, "the
// land"; scene/nodes/land_node.h). A world's land is GLSL that defines
//
//   float landHeight(vec2 p)
//       the ground's height (m) at world (x, z) = p;
//   LandSurface landSurface(LandPoint q)
//       what the ground is there: its albedo (linear), for the ground's
//       shading where no turf covers it (its roughness is the node's), and
//       what grows on it: the cover a grass node standing on the land
//       reads (its sward's density, dryness and height, the canopy over
//       it, how thickly its herbs grow). Start from landSurfaceOf(albedo):
//       bare ground with nothing growing.
//
// and may read landData(i), the node's data (vec4 i of at most 64). Both
// run where the engine makes the land's tiles (land_fill.comp): once a
// texel of a tile as the view needs it, not every frame; the engine works
// out the normal from the heights. A module has every other freedom: its
// own functions, noise (a FastNoise2 tree's GLSL, node.glsl(), is the
// height the CPU reads, land_node.h) and constants. It is compiled with
// this file before it; ground/land_module.glsl is the module of no land the
// built-in fill is compiled with.

struct LandPoint {
    vec2 p;          // world x, z (m)
    float height;    // landHeight(p)
    vec3 normal;     // the ground's, from its heights
};

struct LandSurface {
    vec3 albedo;     // linear
    // What grows here, for a grass node on the land (scene_grass.glsl):
    // x the sward's density (0 none .. 1), y its dryness (0 lush .. 1
    // sun-dried), z its height multiplier (0 .. 2), w how closed the canopy
    // overhead is (0 open .. 1 closed: litter on the turf, the herbs' light).
    vec4 cover;
    float herbs;     // how thickly herbs grow (0 none .. 1 as their abundance has it)
};

// Bare ground of `albedo`: nothing grows (cover density 0, height 1).
LandSurface landSurfaceOf(vec3 albedo) {
    LandSurface s;
    s.albedo = albedo;
    s.cover = vec4(0.0, 0.0, 1.0, 0.0);
    s.herbs = 0.0;
    return s;
}

vec4 landData(int i);
