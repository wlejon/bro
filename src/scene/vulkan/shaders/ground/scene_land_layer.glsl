// The land's module contract (docs/scene-world-architecture.md §13, "the
// land"; scene/nodes/land_node.h). A world's land is GLSL that defines
//
//   float landHeight(vec2 p)
//       the ground's height (m) at world (x, z) = p;
//   LandSurface landSurface(LandPoint q)
//       what the ground is there: its albedo (linear), for the ground's
//       shading where no turf covers it (its roughness is the node's).
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
};

vec4 landData(int i);
