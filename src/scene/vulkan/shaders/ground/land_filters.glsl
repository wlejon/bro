// What a land's surface module reads at its texel's footprint
// (scene_land_layer.glsl includes it; docs/land-api.js, "FILTERED READS").
// The surface runs once a texel, and a texel stands for q.footprint metres
// of ground: every line it draws (snow, trees, rock, wetness) must be the
// share of that ground the line covers, soft and the same every time, never
// a point's yes or no texel by texel. These give it the inputs that way:
//
//   LandStat               a value's mean over the footprint and its spread
//                          (standard deviation) there
//   landShare(v, t)        the share of the footprint where v is over t
//   landGridMean(c, p, w)  channel c's mean and spread over ground w wide
//   landGridCell(p)        the cell of the finest grid read at p (m)
//   landTerrain(q, s, b)   slope, aspect frame, hollow (spur), the height's
//                          spread: the landform at scale s
//   landGrain(...)         a fractal value noise of the body's point, its
//                          octaves under the footprint at their mean and
//                          their spread kept
//
// Each reads at a scale clamped to at least the footprint, so what the
// texel cannot hold is its mean, and what that mean hides is the spread.

// (A loop the land's fill keeps a loop: ground/land_grid.glsl.)
#ifndef LAND_LOOP_N
#define LAND_LOOP_N(n) (n)
#endif

struct LandStat {
    float mean;
    float spread;   // the standard deviation about the mean over the footprint (>= 0)
};

// The sum of two independent values: means added, spreads in quadrature.
LandStat landStatAdd(LandStat a, LandStat b) { return LandStat(a.mean + b.mean, length(vec2(a.spread, b.spread))); }
// k times a value.
LandStat landStatScale(LandStat a, float k) { return LandStat(k * a.mean, abs(k) * a.spread); }

// The share of the footprint where a value of mean v.mean spread v.spread
// is over t: the normal distribution's (smoothstep over two spreads each
// way, within a percent of it), a step where there is no spread. Every
// line a surface draws through it blurs to its mean as the texel widens
// past the line's structure, by however much of that the spread says it
// hides.
float landShare(LandStat v, float t) {
    float s = 2.0 * max(v.spread, 1e-6);
    return smoothstep(-s, s, v.mean - t);
}
float landShare(float mean, float spread, float t) { return landShare(LandStat(mean, spread), t); }

// The footprint the reads below are made at. land_fill.comp's globals:
// landFootprint the module's noise's (FN_FOOTPRINT), landFieldFootprint
// the regional grids' (land_regions.glsl); both the texel's while the
// surface runs. A read over wider ground sets both to its own and back.
float lfSetFootprint(float w) {
    float old = landFootprint;
    landFootprint = w;
#ifdef LAND_GRID
    landFieldFootprint = w;
#endif
    return old;
}

// --- the grid ---------------------------------------------------------------

// Channel c's mean over ground w (at least the texel) wide about p, in
// `spread` its spread there: four reads of the grid a quarter of w each
// way, each a read at half of w (the levels finer than that faded to the
// coarser field, each read the mean of the cells it spans), so a wide read
// is the grid's levels as a pyramid, never a few cells picked out of it.
#ifdef LAND_ROUND
void landFrame(vec3 at, out vec3 up, out vec3 east, out vec3 north) {
    up = normalize(vec3(landOx.x, landOy.x, landOz.x) + at);
    // (North toward the body's +y pole; at a pole, toward +z's side.)
    vec3 n = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) - up * up.y : vec3(0.0, 0.0, 1.0) - up * up.z;
    north = normalize(n);
    east = cross(north, up);
}
vec4 landGridMean(int c, vec3 p, float w, out vec4 spread) {
    float old = lfSetFootprint(0.5 * max(w, landFootprint));
    w = 2.0 * landFootprint;
    vec3 up, e, n;
    landFrame(p, up, e, n);
    // (One read in a loop, the four corners: a read is inlined whole.)
    vec4 s1 = vec4(0.0), s2 = vec4(0.0);
    for (int k = 0; k < LAND_LOOP_N(4); ++k) {
        vec4 v = landGrid(c, p + 0.25 * w * ((k & 1) == 0 ? 1.0 : -1.0) * (k < 2 ? e + n : e - n));
        s1 += v;
        s2 += v * v;
    }
    lfSetFootprint(old);
    vec4 m = 0.25 * s1;
    vec4 q = 0.25 * s2 - m * m;
    spread = sqrt(max(q * (4.0 / 3.0), vec4(0.0)));
    return m;
}
#else
vec4 landGridMean(int c, vec2 p, float w, out vec4 spread) {
    float old = lfSetFootprint(0.5 * max(w, landFootprint));
    w = 2.0 * landFootprint;
    vec4 s1 = vec4(0.0), s2 = vec4(0.0);
    for (int k = 0; k < LAND_LOOP_N(4); ++k) {
        vec4 v = landGrid(c, p + 0.25 * w * ((k & 1) == 0 ? 1.0 : -1.0) * (k < 2 ? vec2(1.0, 1.0) : vec2(1.0, -1.0)));
        s1 += v;
        s2 += v * v;
    }
    lfSetFootprint(old);
    vec4 m = 0.25 * s1;
    vec4 q = 0.25 * s2 - m * m;
    spread = sqrt(max(q * (4.0 / 3.0), vec4(0.0)));
    return m;
}
#endif

// The cell (m) of the finest grid the land reads at p for the texel: the
// regional levels' as far as each is in (landGridCover), the global grid's
// past them. A surface reading a shape the grid's cells make (a valley a
// D8 network cut, cell by cell) reads it at a scale of a cell or more.
#ifdef LAND_ROUND
float landGridCell(vec3 p)
#else
float landGridCell(vec2 p)
#endif
{
#ifdef LAND_GRID
    float cell = landGridCell();
#ifdef LAND_REGIONS
    for (int L = LAND_LOOP_N(LAND_REGION_LEVELS - 1); L >= 0; --L) cell = mix(cell, landRegionCell(L), landGridCover(L, p));
#endif
    return cell;
#else
    return 1e30;
#endif
}

// --- the landform -------------------------------------------------------------

// The terrain's attributes at scale `scale` (m): read from the land's
// heights a scale either way along the ground (each a read at half the
// scale: the module's noise and the regional grids faded to their mean
// past that, so the heights are the ground as that scale sees it), the
// scale at least 1.5 texels. Heights under `base` read as base (a sea's
// level: a coast's ring on the sea floor stood every coast up as a crest).
struct LandTerrain {
    vec2 slope;       // the rise (m/m) along east, north: the mean slope over the footprint
    float grade;      // its length
    float hollow;     // the ring's mean less the centre (m) at `scale`: > 0 a hollow, gully or
                      // valley, < 0 a spur, crest or ridge (its relief over its surroundings
                      // is -hollow). Where the footprint is wider than the scale, its mean over
                      // the footprint: the read's at the wider scale times (scale / read)^2 (a
                      // ring difference goes as the scale squared times the curvature), so a
                      // far texel's hollow is the few metres its broad form says, not the
                      // whole relief of the ground it spans
    LandStat height;  // the heights' mean over the read and their spread (the relief there)
    vec3 east, north; // the ground's frame: on a round body unit vectors along it (north toward
                      // the body's +y pole), on a flat one (1, 0, 0) and (0, 0, 1) of (x, ?, z)
    float scale;      // the scale read (m): `scale`, or 1.5 texels where that is wider
};

#ifdef LAND_ROUND
float lfHeightAt(LandPoint q, vec3 e, vec3 n, vec2 o, float base) {
    return max(landHeight(q.at + e * o.x + n * o.y), base);
}
#else
float lfHeightAt(LandPoint q, vec3 e, vec3 n, vec2 o, float base) { return max(landHeight(q.p + o), base); }
#endif

LandTerrain landTerrain(LandPoint q, float scale, float base) {
    LandTerrain t;
    float s = max(scale, 1.5 * q.footprint);
    t.scale = s;
#ifdef LAND_ROUND
    vec3 up;
    landFrame(q.at, up, t.east, t.north);
#else
    t.east = vec3(1.0, 0.0, 0.0);
    t.north = vec3(0.0, 0.0, 1.0);
#endif
    float old = lfSetFootprint(max(0.5 * s, q.footprint));
    // The centre and the four a scale east, west, north and south of it,
    // one height read in a loop (a read is the world's whole tree: five
    // inlined, twice a surface, were most of a fill's compile).
    float hc = 0.0, ring = 0.0, sum2 = 0.0;
    vec2 rise = vec2(0.0);
    for (int i = 0; i < LAND_LOOP_N(5); ++i) {
        vec2 dir = i == 0 ? vec2(0.0) : i == 1 ? vec2(1.0, 0.0) : i == 2 ? vec2(-1.0, 0.0) : i == 3 ? vec2(0.0, 1.0) : vec2(0.0, -1.0);
        float hi = lfHeightAt(q, t.east, t.north, s * dir, base);
        if (i == 0) hc = hi;
        else ring += hi;
        rise += hi * dir;
        sum2 += hi * hi;
    }
    lfSetFootprint(old);
    t.slope = rise / (2.0 * s);
    t.grade = length(t.slope);
    float k = scale / s;
    t.hollow = (0.25 * ring - hc) * k * k;
    float m = 0.2 * (hc + ring);
    float v = 0.2 * sum2 - m * m;
    // (Less the plane the slope accounts for: the spread is the relief
    // about the ground's slope, not the slope's rise across the read.)
    v -= 0.4 * dot(t.slope, t.slope) * s * s;
    t.height = LandStat(m, sqrt(max(v * 1.25, 0.0)));
    return t;
}

// --- noise --------------------------------------------------------------------

// Value noise (-1 .. 1, mean 0) in cells of 1, of a point in 3D.
float lfHash(ivec3 i) {
    uint h = uint(i.x) * 0x8da6b343u ^ uint(i.y) * 0xd8163841u ^ uint(i.z) * 0xcb1ab31fu;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return float(h) * (2.0 / 4294967295.0) - 1.0;
}
float landValueNoise(vec3 p) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(lfHash(i), lfHash(i + ivec3(1, 0, 0)), f.x), mix(lfHash(i + ivec3(0, 1, 0)), lfHash(i + ivec3(1, 1, 0)), f.x), f.y),
               mix(mix(lfHash(i + ivec3(0, 0, 1)), lfHash(i + ivec3(1, 0, 1)), f.x), mix(lfHash(i + ivec3(0, 1, 1)), lfHash(i + ivec3(1, 1, 1)), f.x), f.y),
               f.z);
}

// The body's point (m) of a surface point: on a round body the datum's
// (float: a metre's precision at an Earth's radius, for noises of tens of
// metres and up), on a flat one (x, 0, z).
#ifdef LAND_ROUND
vec3 landBodyPoint(vec3 at) { return vec3(landOx.x, landOy.x, landOz.x) + at; }
#endif
vec3 landBodyPoint(vec2 p) { return vec3(p.x, 0.0, p.y); }

// A fractal value noise of body point P (m): `octaves` octaves from
// `wavelength` (m) down, each `lacunarity` of the last's wavelength and
// `gain` of its amplitude, `seed` shifting it. Each octave fades to its
// mean (0) as the footprint (the texel's, or `scale` where that is wider)
// passes 0.3 of its wavelength to 0.6, so the finest it holds is about two
// texels a wave. mean: the held octaves, scaled so that all of them
// together have a spread of about 1; spread: the share of that the
// footprint does not hold. A line cut by it (landShare(grain, t)) is crisp
// to the texel where the texel holds the grain and the share of each side
// where it does not.
LandStat landGrain(vec3 P, float seed, float wavelength, int octaves, float lacunarity, float gain, float scale) {
    float fp = max(landFootprint, scale);
    float s = 0.0, l = wavelength, a = 1.0, vr = 0.0, vt = 0.0;
    for (int k = 0; k < LAND_LOOP_N(octaves); ++k) {
        float w = 1.0 - smoothstep(0.3 * l, 0.6 * l, fp);
        if (w > 0.0) s += a * w * landValueNoise(P / l + seed + float(k) * 23.9);
        vr += a * a * (1.0 - w * w);
        vt += a * a;
        l *= lacunarity;
        a *= gain;
    }
    return LandStat(s * 3.0 / sqrt(vt), sqrt(vr / vt));
}
