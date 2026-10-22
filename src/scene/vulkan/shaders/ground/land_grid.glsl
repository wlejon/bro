// The land's processes' grid (scene/world/land_grid.h), read the way the
// CPU's LandGrid::sample reads it. Its layout comes from
// ground/land_grid_config.glsl (LandGrid::glslConfig; empty with no
// processes, when nothing here is defined); the includer defines
//
//   vec4 landGridLoad(ivec3 t)
//       texel t.xy (a root's cells from 1, its halo 0 and cells + 1) of
//       layer t.z (root * LAND_GRID_CHANNELS + channel)
//
// before it.

#ifdef LAND_GRID

// A cell's side in chart metres.
float landGridCell() { return 2.0 * LAND_GRID_HALF / float(LAND_GRID_CELLS); }

// Channel `c` at chart point `x` of `root`: bilinear between the cell
// centres either side (texel i's at -half + (i - 0.5) cells), the halo's
// included, held at the halo's centres past them.
vec4 landGridAt(int c, int root, vec2 x) {
    vec2 s = (x + LAND_GRID_HALF) / landGridCell() + 0.5;
    ivec2 i = clamp(ivec2(floor(s)), ivec2(0), ivec2(LAND_GRID_CELLS));
    vec2 f = clamp(s - vec2(i), 0.0, 1.0);
    int layer = clamp(root, 0, LAND_GRID_ROOTS - 1) * LAND_GRID_CHANNELS + clamp(c, 0, LAND_GRID_CHANNELS - 1);
    vec4 a = landGridLoad(ivec3(i, layer)), b = landGridLoad(ivec3(i + ivec2(1, 0), layer));
    vec4 d = landGridLoad(ivec3(i + ivec2(0, 1), layer)), e = landGridLoad(ivec3(i + ivec2(1, 1), layer));
    vec4 lo = a + (b - a) * f.x, hi = d + (e - d) * f.x;
    return lo + (hi - lo) * f.y;
}

#ifdef LAND_GRID_ROUND
// The cube's faces (world/place.cpp's): normal, and the axes its chart's x
// and z run along.
const vec3 kLandFaceN[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1),
                                   vec3(0, 0, -1));
const vec3 kLandFaceU[6] = vec3[6](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0),
                                   vec3(-1, 0, 0));
const vec3 kLandFaceV[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, 1, 0),
                                   vec3(0, 1, 0));

// The face body direction `d` lies on (the first of the nearest, as chartOf).
int landGridFace(vec3 d) {
    int f = 0;
    float best = -2.0;
    for (int k = 0; k < 6; ++k) {
        float c = dot(d, kLandFaceN[k]);
        if (c > best) {
            best = c;
            f = k;
        }
    }
    return f;
}

// Its chart point on face `f`, and back.
vec2 landGridChart(vec3 d, int f) {
    float n = dot(d, kLandFaceN[f]);
    return LAND_GRID_RADIUS * vec2(atan(dot(d, kLandFaceU[f]), n), atan(dot(d, kLandFaceV[f]), n));
}
vec3 landGridDir(int f, vec2 x) {
    vec2 t = tan(x / LAND_GRID_RADIUS);
    return normalize(kLandFaceN[f] + t.x * kLandFaceU[f] + t.y * kLandFaceV[f]);
}

// Channel `c` at body direction `d` (unit).
vec4 landGridAtDir(int c, vec3 d) {
    int f = landGridFace(d);
    return landGridAt(c, f, landGridChart(d, f));
}
#endif

#endif
