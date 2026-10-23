// The land's module contract (docs/scene-world-architecture.md §13, "the
// land"; scene/nodes/land_node.h). A world's land is GLSL that defines
//
//   float landHeight(vec2 p)
//       the ground's height (m) at world (x, z) = p, on a flat body;
//   float landHeight(vec3 p)
//       on a round one (docs/scene-world-architecture.md §11): its height
//       over the datum at the datum's point p, relative to the fill's origin
//       (a double point the fill sets per tile: a FastNoise2 tree's GLSL
//       reads it as FN_ORIGIN3, so its fine octaves hold 6371 km out, and
//       the module's own trees may too);
//   LandSurface landSurface(LandPoint q)
//       what the ground is there: its albedo (linear), for the ground's
//       shading where no turf covers it (its roughness is the node's), and
//       what grows on it: the cover a grass node standing on the land
//       reads (its sward's density, dryness and height, the canopy over
//       it, how thickly its herbs grow). Start from landSurfaceOf(albedo):
//       bare ground with nothing growing.
//
// and may read landData(i), the node's data (vec4 i of at most 64), and
// landGrid(c, p), its processes' grid (below). Both
// run where the engine makes the land's tiles (land_fill.comp): once a
// texel of a tile as the view needs it, not every frame; the engine works
// out the normal from the heights. A module has every other freedom: its
// own functions, noise (a FastNoise2 tree's GLSL, node.glsl(), is the
// height the CPU reads, land_node.h) and constants. It is compiled with
// this file before it; ground/land_module.glsl is the module of no land the
// built-in fill is compiled with.

struct LandPoint {
    vec2 p;          // world x, z (m); on a round body its chart's (a cube face's metres, coarse)
    float height;    // landHeight(p) (on a round body landHeight(at))
    vec3 normal;     // the ground's, from its heights: y up (on a round body in the frame of its
                     // up, x along its face's u, z its v)
    vec3 at;         // on a round body the datum's point under it, relative to the origin; else (p.x, 0, p.y)
    vec3 up;         // the body's up there (the body's frame; (0, 1, 0) flat)
    float footprint; // the texel's width (m): the ground it stands for. Its height
                     // is the noise's mean over it, so what the surface decides from
                     // the height (a snowline, a tree line) is a share of it there,
                     // its threshold widened by the relief the texel hides, not all
                     // or nothing texel by texel
};

// A round body's land (the fill defines LAND_ROUND): each tile's texels
// relative to an origin near them, a double-float a body axis.
#ifdef LAND_ROUND
vec2 landOx, landOy, landOz;
#define FN_ORIGIN3 FnO3(landOx, landOy, landOz)
#endif

// The land's processes' grid (ground/land_process_layer.glsl): channel c
// (a vec4) at world (x, z) = p on a flat body, at the datum's point p
// (relative to the fill's origin, as landHeight's) on a round one; zero
// with no processes. Where its regional grids are in (scene/world/
// land_regions.h), their channel blended over the global grid's
// (ground/land_regions.glsl). Channel 0 is the land's height over its
// tree's: a module's landHeight is landGrid(0, p).x plus landGrid(0, p).y
// times its tree (its tree alone with no processes), as the CPU's heights
// are (scene/world/land_grid.h's LandBase).
vec4 landGrid(int c, vec2 p) {
#if defined(LAND_GRID) && !defined(LAND_GRID_ROUND)
    return landFieldAt(c, p);
#else
    return vec4(0.0);
#endif
}
vec4 landGrid(int c, vec3 p) {
#if defined(LAND_GRID_ROUND) && defined(LAND_ROUND)
    return landFieldDir(c, normalize(vec3(landOx.x, landOy.x, landOz.x) + p), p);
#else
    return vec4(0.0);
#endif
}
// How much of landGrid's kept channels at p are its regional grids' of
// `level` or finer (0 .. 1): 0 where none is in, or past the reach of a
// texel this coarse (a tile reads a level only while its texel is no wider
// than the level's cells). What the world's relief lacks there, it can know
// it lacks: a surface shading a band laid down on a level's regions spreads
// its lines by the band's amplitude where the level is not in, so the land
// past the regions looks as the land in them does at a distance.
float landGridCover(int level, vec2 p) {
#if defined(LAND_GRID) && !defined(LAND_GRID_ROUND)
    return landFieldCoverAt(level, p);
#else
    return 0.0;
#endif
}
float landGridCover(int level, vec3 p) {
#if defined(LAND_GRID_ROUND) && defined(LAND_ROUND)
    return landFieldCoverDir(level, normalize(vec3(landOx.x, landOy.x, landOz.x) + p), p);
#else
    return 0.0;
#endif
}

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
