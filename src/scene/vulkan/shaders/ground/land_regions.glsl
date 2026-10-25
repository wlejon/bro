// A land's regional grids (scene/world/land_regions.h), read the way the
// CPU's LandRegionLattice::blendDir / blendAt reads them: every level's
// regions blended over the next coarser field, the global grid's
// (ground/land_grid.glsl) at the top. Its layout comes from
// ground/land_region_config.glsl (LandRegionLattice::glslConfig; empty with
// no regions, when only the global grid is read); the includer defines
//
//   vec4 landRegionLoad(ivec3 t)
//       texel t.xy (from 0) of layer t.z (slot * LAND_REGION_KEEP + channel)
//       of the regions' slots;
//   uvec4 landRegionEntry(uint i)
//       entry i of the regions' table: x the region's key
//       (LandRegionKey::packed, 0 an empty entry), y its slot;
//
// and ground/land_grid.glsl before it.
//
//   vec4 landFieldDir(int c, vec3 d, vec3 p)
//                                         channel c at body direction d (round; p for
//                                         an includer's exact charts, landChartOf)
//   vec4 landFieldAt(int c, vec2 x)       channel c at chart point x (flat)
//   ...From(c, p, minLevel)               only the levels from minLevel up
//   float landFieldCoverDir(int level, vec3 d, vec3 p) / landFieldCoverAt(level, x)
//                                         the share of the value from levels 0 to
//                                         `level` (0 where none is in)

#ifdef LAND_GRID

// The width (m) of what the reader's texel stands for (land_fill.comp sets
// it a texel; 0, the default, reads every level whole, as the CPU does).
float landFieldFootprint = 0.0;

// The global grid read at the reader's texel: a texel wider than its cells
// reads their mean over it (landGridTexel), as the regions' do.
vec4 landGridRead(int c, int root, vec2 x) { return landGridTexel(c, root, x, landFieldFootprint / landGridCell()); }
#ifdef LAND_GRID_ROUND
vec4 landGridReadDir(int c, vec3 d) {
    vec4 r;
    LAND_GRID_DIR(landGridRead, c, d, r)
    return r;
}
#endif

#ifdef LAND_REGIONS

float landRegionCell(int L) { return LAND_REGION_CELL0 * pow(LAND_REGION_RATIO, float(L)); }
float landRegionStep(int L) { return float(LAND_REGION_CELLS - 2 * LAND_REGION_APRON) * landRegionCell(L); }

uint landRegionKey(int L, int root, int i, int j) {
    return 1u + (uint(L) | (uint(root) << 3) | (uint(i + 1024) << 6) | (uint(j + 1024) << 17));
}

// The region's slot (-1: not in).
int landRegionSlot(uint key) {
    uint h = (key * 2654435761u) >> (32 - LAND_REGION_TABLE_BITS);
    for (int k = 0; k < 16; ++k) {
        uvec4 e = landRegionEntry((h + uint(k)) & uint(LAND_REGION_TABLE - 1));
        if (e.x == key) return int(e.y);
        if (e.x == 0u) return -1;
    }
    return -1;
}

// The lattice indices a root's regions take at level L.
void landRegionRange(int L, out int lo, out int hi) {
    float step = landRegionStep(L);
#ifdef LAND_GRID_ROUND
    float band = float(LAND_REGION_APRON) * landRegionCell(L);
#else
    float band = 0.0;
#endif
    lo = max(int(floor(-band / step)), -1024);
    hi = min(int(floor((2.0 * LAND_GRID_HALF + band) / step)), 1023);
}

// The cell of the field next coarser than level L: level L + 1's, or the
// global grid's over the coarsest level.
float landRegionCoarserCell(int L) {
    return L + 1 < LAND_REGION_LEVELS ? landRegionCell(L + 1) : landGridCell();
}

// How much of level L a texel of landFieldFootprint (m) reads: all of it
// until the texel is half the coarser field's cell, none once it is that
// cell, so a coarse tile holds the regions' detail it can carry, as the
// module's noise does. (A texel wider than the level's cells reads their
// mean over it, landRegionTexel. Faded out from the texel at the level's
// own cell, a texel between it and the coarser field's read the coarser
// field alone, as many texels a cell as the levels' ratio: from orbit the
// global grid's cells, a fifth of level 3's, stood as staircases along
// every coast and river, while the tiles held from nearer views showed the
// regions' shapes beside them.)
float landRegionFade(int L) {
    if (!(landFieldFootprint > 0.0)) return 1.0;
    float cell = landRegionCell(L);
    float R = max(landRegionCoarserCell(L) / cell, 2.0);
    return 1.0 - smoothstep(0.5 * R, R, landFieldFootprint / cell);
}

// Whether a level is read only about its centre (scene/world/land_regions.h,
// PRESENCE): the land's reads; a region's run reads the coarser field
// without it (land_region_layer.glsl clears it).
bool landFieldPresence = true;

// Level L's centre, from the words past the table's entries: two a level,
// (its body direction, 1 when it has one) and (inner, outer, its chart x, z).
vec4 landRegionCentreWord(int L, int k) {
    return uintBitsToFloat(landRegionEntry(uint(LAND_REGION_TABLE + 2 * L + k)));
}
// How much of level L is read at body direction d (round) or chart point x
// (flat): 1 - smoothstep(inner, outer, its distance along the ground from
// the level's centre), none from outer on (both 0 while none of the level is
// whole about the eye).
float landRegionPresence(int L, vec3 d, vec2 x) {
    if (!landFieldPresence) return 1.0;
    vec4 a = landRegionCentreWord(L, 0);
    if (a.w < 0.5) return 1.0;
    vec4 b = landRegionCentreWord(L, 1);
#ifdef LAND_GRID_ROUND
    float dist = 2.0 * LAND_GRID_RADIUS * asin(min(0.5 * length(d - a.xyz), 1.0));
#else
    float dist = length(x - b.zw);
#endif
    if (dist >= b.y) return 0.0;
    return 1.0 - smoothstep(b.x, b.y, dist);
}

// A region's weight along one axis at texel coordinate t.
float landRegionWeight(float t) {
    float w = 2.0 * float(LAND_REGION_APRON);
    return smoothstep(0.0, w, t) * smoothstep(0.0, w, float(LAND_REGION_CELLS) - t);
}

// Channel c of the region in `slot` at texel coordinate t: the cubic
// B-spline over its texels (landBspline, as the global grid's), held
// inside them.
vec4 landRegionCubic(int slot, int c, vec2 t) {
    t = clamp(t - 0.5, vec2(0.0), vec2(float(LAND_REGION_CELLS - 1)));
    ivec2 i = ivec2(floor(t));
    vec2 f = t - vec2(i);
    vec4 wx = landBspline(f.x), wy = landBspline(f.y);
    int layer = slot * LAND_REGION_KEEP + c;
    ivec4 xs = clamp(i.x + ivec4(-1, 0, 1, 2), 0, LAND_REGION_CELLS - 1);
    vec4 sum = vec4(0.0);
    for (int b = 0; b < 4; ++b) {
        int y = clamp(i.y - 1 + b, 0, LAND_REGION_CELLS - 1);
        sum += wy[b] * (wx.x * landRegionLoad(ivec3(xs.x, y, layer)) + wx.y * landRegionLoad(ivec3(xs.y, y, layer)) +
                        wx.z * landRegionLoad(ivec3(xs.z, y, layer)) + wx.w * landRegionLoad(ivec3(xs.w, y, layer)));
    }
    return sum;
}

// The same over a reader's texel `q` of the region's texels wide: one read
// while q is at most one, else the mean of four a quarter of q off the
// point each way, so a coarse tile's texel takes the cells it spans, never
// the one its centre lands on (a river a cell wide read every few cells
// was dotted).
vec4 landRegionTexel(int slot, int c, vec2 t, float q) {
    if (q <= 1.0) return landRegionCubic(slot, c, t);
    float o = 0.25 * q;
    return 0.25 * (landRegionCubic(slot, c, t + vec2(-o, -o)) + landRegionCubic(slot, c, t + vec2(o, -o)) +
                   landRegionCubic(slot, c, t + vec2(-o, o)) + landRegionCubic(slot, c, t + vec2(o, o)));
}

// Level L of one root at chart point s: its regions' weighted values (times
// wf) added to sum, their weights to cover. The point is given from the
// root's corner (chart + half) as a double-float, s.xy + s.zw: a chart's
// metres run to millions, where a float's step is half a metre and more, and
// a region's cells are tens of metres, so a float chart point would read
// them in steps (the ground terraced in treads and risers).
void landRegionLevel(int c, int L, int root, vec4 s, float wf, inout vec4 sum, inout float cover) {
    float step = landRegionStep(L), cell = landRegionCell(L);
    vec2 u = (s.xy + s.zw) / step;
    float a = float(LAND_REGION_APRON) / float(LAND_REGION_CELLS - 2 * LAND_REGION_APRON);
    int lo, hi;
    landRegionRange(L, lo, hi);
    float q = landFieldFootprint / cell;
    ivec2 l = max(ivec2(floor(u - a)), ivec2(lo)), h = min(ivec2(floor(u + a)), ivec2(hi));
    for (int dj = 0; dj < 2; ++dj) {
        int j = l.y + dj;
        if (j > h.y) break;
        for (int di = 0; di < 2; ++di) {
            int i = l.x + di;
            if (i > h.x) break;
            int slot = landRegionSlot(landRegionKey(L, root, i, j));
            if (slot < 0) continue;
            // (The region's first texel's corner from the root's is a whole
            // number of cells; the point's high part less its lattice step
            // is exact, the low part added after.)
            precise vec2 r = s.xy - vec2(i, j) * step;
            precise vec2 t = (r + float(LAND_REGION_APRON) * cell + s.zw) / cell;
            float w = wf * landRegionWeight(t.x) * landRegionWeight(t.y);
            if (w <= 0.0) continue;
            sum += w * landRegionTexel(slot, c, t, q);
            cover += w;
        }
    }
}

// (Each level is blended over the coarser value, value = coarser (1 - cover)
// + its sum; worked from the finest level out, the coarser ones are read
// only while some of the value is still theirs: where the finest regions
// cover a point, they are all that is read.)
#ifdef LAND_GRID_ROUND
// Face f's chart point from its corner (chart + half) as a double-float
// (xy + zw) at body direction d, the point p (the includer's: an offset from
// a point whose charts it knows exactly). An includer that has them defines
// LAND_CHART_EXACT and this function; else the float chart's.
#ifdef LAND_CHART_EXACT
vec4 landChartExact(int f, vec3 d, vec3 p);
#endif
vec4 landChartOf(int f, vec3 d, vec3 p) {
#ifdef LAND_CHART_EXACT
    return landChartExact(f, d, p);
#else
    return vec4(landGridChart(d, f) + LAND_GRID_HALF, 0.0, 0.0);
#endif
}

// Levels minLevel to endLevel - 1 of channel c at d: their share of the
// value, and in `rest` the share still the coarser field's.
vec4 landFieldDirLevels(int c, vec3 d, vec3 p, int minLevel, int endLevel, int f0, vec2 x0, out float rest) {
    float m0 = max(abs(x0.x), abs(x0.y));
    vec4 acc = vec4(0.0);
    rest = 1.0;   // the share still the coarser levels'
    for (int L = minLevel; L < min(endLevel, LAND_REGION_LEVELS) && rest > 0.0; ++L) {
        float fade = landRegionFade(L) * landRegionPresence(L, d, vec2(0.0));
        if (fade <= 0.0) continue;
        float band = float(LAND_REGION_APRON) * landRegionCell(L);
        vec4 sum = vec4(0.0);
        float cover = 0.0;
        // The faces about the point, weighed by how far inside each it lies
        // (its own alone away from its edges). (One loop, one call: the
        // fill inlines this at every read.)
        float wsum = 0.0;
        int first = m0 < LAND_GRID_HALF - band ? 6 : 0;
        for (int k = first; k < 7; ++k) {
            int f = k == 6 ? f0 : k;
            if (k < 6 && (f == f0 || dot(d, kLandFaceN[f]) < 0.2)) continue;
            vec2 x = f == f0 ? x0 : landGridChart(d, f);
            float m = max(abs(x.x), abs(x.y));
            float wf = first == 6 ? 1.0 : 1.0 - smoothstep(LAND_GRID_HALF - band, LAND_GRID_HALF + band, m);
            if (wf <= 0.0) continue;
            vec4 s = vec4(0.0);
            float cv = 0.0;
            landRegionLevel(c, L, f, landChartOf(f, d, p), wf, s, cv);
            sum += s;
            cover += cv;
            wsum += wf;
        }
        if (wsum > 0.0) {
            acc += rest * fade * sum / wsum;
            rest *= 1.0 - cover * fade / wsum;
        }
    }
    return acc;
}
vec4 landFieldDirFrom(int c, vec3 d, vec3 p, int minLevel) {
    if (c >= LAND_REGION_KEEP) return landGridReadDir(c, d);
    // The face the point is on; the others only within a band of its edge.
    int f0 = landGridFace(d);
    vec2 x0 = landGridChart(d, f0);
    float rest;
    vec4 acc = landFieldDirLevels(c, d, p, minLevel, LAND_REGION_LEVELS, f0, x0, rest);
    if (rest > 0.0) acc += rest * landGridReadDir(c, d);
    return acc;
}
vec4 landFieldDir(int c, vec3 d, vec3 p) { return landFieldDirFrom(c, d, p, 0); }
float landFieldCoverDir(int level, vec3 d, vec3 p) {
    int f0 = landGridFace(d);
    float rest;
    landFieldDirLevels(0, d, p, 0, level + 1, f0, landGridChart(d, f0), rest);
    return 1.0 - rest;
}
#else
vec4 landFieldAtLevels(int c, vec2 x, int minLevel, int endLevel, out float rest) {
    vec4 acc = vec4(0.0);
    rest = 1.0;
    for (int L = minLevel; L < min(endLevel, LAND_REGION_LEVELS) && rest > 0.0; ++L) {
        float fade = landRegionFade(L) * landRegionPresence(L, vec3(0.0), x);
        if (fade <= 0.0) continue;
        vec4 sum = vec4(0.0);
        float cover = 0.0;
        landRegionLevel(c, L, 0, vec4(x + LAND_GRID_HALF, 0.0, 0.0), 1.0, sum, cover);
        acc += rest * fade * sum;
        rest *= 1.0 - cover * fade;
    }
    return acc;
}
vec4 landFieldAtFrom(int c, vec2 x, int minLevel) {
    if (c >= LAND_REGION_KEEP) return landGridRead(c, 0, x);
    float rest;
    vec4 acc = landFieldAtLevels(c, x, minLevel, LAND_REGION_LEVELS, rest);
    if (rest > 0.0) acc += rest * landGridRead(c, 0, x);
    return acc;
}
vec4 landFieldAt(int c, vec2 x) { return landFieldAtFrom(c, x, 0); }
float landFieldCoverAt(int level, vec2 x) {
    float rest;
    landFieldAtLevels(0, x, 0, level + 1, rest);
    return 1.0 - rest;
}
#endif

#else   // no regions: the global grid alone

#ifdef LAND_GRID_ROUND
vec4 landFieldDirFrom(int c, vec3 d, vec3 p, int minLevel) { return landGridReadDir(c, d); }
vec4 landFieldDir(int c, vec3 d, vec3 p) { return landGridReadDir(c, d); }
float landFieldCoverDir(int level, vec3 d, vec3 p) { return 0.0; }
#else
vec4 landFieldAtFrom(int c, vec2 x, int minLevel) { return landGridRead(c, 0, x); }
vec4 landFieldAt(int c, vec2 x) { return landGridRead(c, 0, x); }
float landFieldCoverAt(int level, vec2 x) { return 0.0; }
#endif

#endif

#endif
