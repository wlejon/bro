// The land's processes' grid (scene/world/land_grid.h), read the way the
// CPU's LandGrid::sample reads it. Its layout comes from
// ground/land_grid_config.glsl (LandGrid::glslConfig; empty with no
// processes, when nothing here is defined); the includer defines
//
//   vec4 landGridLoad(ivec3 t)
//       texel t.xy (a root's cells from 1, its halo 0 and cells + 1) of
//       layer t.z (root * LAND_GRID_CHANNELS + channel)
//
// before it. An includer that inlines the reads many times over (the land's
// fill) may define LAND_LOOP_N(n), the count of each of their loops (over
// faces, levels, regions, heights), as n plus a zero the compiler cannot
// know, so that each stays one loop, not unrolled at every read.
#ifndef LAND_LOOP_N
#define LAND_LOOP_N(n) (n)
#endif

#ifdef LAND_GRID

// A cell's side in chart metres.
float landGridCell() { return 2.0 * LAND_GRID_HALF / float(LAND_GRID_CELLS); }

// The uniform cubic B-spline's four weights at fraction f between the
// middle two of four cells (scene/world/land_grid.h's landBspline): a read
// through them is C2, so the slopes and curvatures of a field read from
// cells (or of heights built on one) are continuous across the cells'
// edges, never the cells' facets and creases a bilinear read draws. It
// smooths rather than interpolates: at a cell's centre it reads
// (a + 4 b + c) / 6 along each axis.
vec4 landBspline(float f) {
    float g = 1.0 - f, f2 = f * f, f3 = f2 * f;
    return vec4(g * g * g, 3.0 * f3 - 6.0 * f2 + 4.0, -3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0, f3) * (1.0 / 6.0);
}

// Channel `c` at chart point `x` of `root`: the cubic B-spline over the
// four cell centres about it each way (texel i's at -half + (i - 0.5)
// cells), the halo's included, held at the halo's past them.
vec4 landGridAt(int c, int root, vec2 x) {
    vec2 s = clamp((x + LAND_GRID_HALF) / landGridCell() + 0.5, vec2(0.0), vec2(float(LAND_GRID_CELLS + 1)));
    ivec2 i = ivec2(floor(s));
    vec2 f = s - vec2(i);
    vec4 wx = landBspline(f.x), wy = landBspline(f.y);
    int layer = clamp(root, 0, LAND_GRID_ROOTS - 1) * LAND_GRID_CHANNELS + clamp(c, 0, LAND_GRID_CHANNELS - 1);
    ivec4 xs = clamp(i.x + ivec4(-1, 0, 1, 2), 0, LAND_GRID_CELLS + 1);
    vec4 sum = vec4(0.0);
    for (int b = 0; b < LAND_LOOP_N(4); ++b) {
        int y = clamp(i.y - 1 + b, 0, LAND_GRID_CELLS + 1);
        sum += wy[b] * (wx.x * landGridLoad(ivec3(xs.x, y, layer)) + wx.y * landGridLoad(ivec3(xs.y, y, layer)) +
                        wx.z * landGridLoad(ivec3(xs.z, y, layer)) + wx.w * landGridLoad(ivec3(xs.w, y, layer)));
    }
    return sum;
}

// The same over a reader's texel `q` cells wide: one read while q is at
// most one, else the mean of four a quarter of q off the point each way, so
// a coarse texel takes the cells it spans (land_regions.glsl's
// landFieldFootprint; a far tile's texel of 48 km over 20 km cells).
vec4 landGridTexel(int c, int root, vec2 x, float q) {
    // (One read in a loop, once or four times.)
    int n = q <= 1.0 ? 1 : 4;
    float o = n == 1 ? 0.0 : 0.25 * q * landGridCell();
    vec4 sum = vec4(0.0);
    for (int k = 0; k < n + LAND_LOOP_N(0); ++k)
        sum += landGridAt(c, root, x + o * vec2((k & 1) == 0 ? -1.0 : 1.0, k < 2 ? -1.0 : 1.0));
    return sum / float(n);
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
    for (int k = 0; k < LAND_LOOP_N(6); ++k) {
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

// How much face `f`'s read at chart point x is weighed in at body
// direction d: all of it within its square less a cell, none a cell past
// it. (A read through the B-spline reaches two cells about the point, the
// halo only one past the face: near an edge each face's read clamps at
// its halo, and the face a point lies on changed there with a step.
// Within a cell of an edge the faces about the point are cross-faded.)
float landGridFaceWeight(vec2 x) {
    float c = landGridCell();
    return 1.0 - smoothstep(LAND_GRID_HALF - c, LAND_GRID_HALF + c, max(abs(x.x), abs(x.y)));
}

// Channel `c` at body direction `d` (unit), its root's chart read through
// `read` (landGridAt, or a texel's mean): on the face it lies on, the
// faces about it cross-faded within a cell of an edge. (One read in one
// loop, k_ 6 the point's own face, the only one away from the edges: a read
// is inlined whole wherever it is written.)
#define LAND_GRID_DIR(read, c, d, result)                                                         \
    {                                                                                            \
        int f0_ = landGridFace(d);                                                               \
        vec2 x0_ = landGridChart(d, f0_);                                                        \
        int first_ = max(abs(x0_.x), abs(x0_.y)) < LAND_GRID_HALF - landGridCell() ? 6 : 0;      \
        vec4 s_ = vec4(0.0);                                                                     \
        float w_ = 0.0;                                                                          \
        for (int k_ = first_; k_ < LAND_LOOP_N(7); ++k_) {                                       \
            int f_ = k_ == 6 ? f0_ : k_;                                                         \
            if (k_ < 6 && (f_ == f0_ || dot(d, kLandFaceN[f_]) < 0.2)) continue;                 \
            vec2 x_ = f_ == f0_ ? x0_ : landGridChart(d, f_);                                    \
            float wk_ = first_ == 6 ? 1.0 : landGridFaceWeight(x_);                              \
            if (wk_ <= 0.0) continue;                                                            \
            s_ += wk_ * read(c, f_, x_);                                                         \
            w_ += wk_;                                                                           \
        }                                                                                        \
        result = s_ / w_;   /* (the point's own face is at least half in) */                     \
    }
vec4 landGridAtDir(int c, vec3 d) {
    vec4 r;
    LAND_GRID_DIR(landGridAt, c, d, r)
    return r;
}
#endif

#endif
