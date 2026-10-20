// The forest's GPU planner (scene_forest_gpu_plan.cpp): the buffers of
// scene_forest_plan_layout.h, and the drawer's rules for a tree, each the
// GLSL of its C++ in scene_mesh_drawer_forest.cpp and scene/forest_node.cpp,
// line for line: ForestNode::treeLod, fadeRanges and fadeCode, the plan's
// casterLod, clumpPx and addTree, and fitDetail's search. They must stay
// so: what the GPU path draws is the CPU path's choice, tree by tree.
//
// The arithmetic is the CPU's too. Every sum and product a rule compares or
// stores is `precise` (no fused multiply-add the CPU does not do), and a
// division, square root or hypot goes through fdiv, fsqrt and fhypot, which
// round as IEEE does: Vulkan lets a GLSL `/` be 2.5 ulp out and `sqrt`
// inversesqrt's error, and a tree on a hand-over's edge would take the
// other tier than the CPU's.
//
// A stage includes this after its #version line and its own defines.

#include "scene_forest_layout.h"
#include "scene_forest_plan_layout.h"

layout(std430, set = 0, binding = 0) readonly buffer PlanStatic { uint w[]; } planStatic;
layout(std430, set = 0, binding = 1) readonly buffer PlanParams { uint w[]; } planParams;
layout(std430, set = 0, binding = 2) buffer PlanWork { uint w[]; } planWork;
layout(std430, set = 0, binding = 3) buffer PlanShared { uint w[]; } planShared;
layout(std430, set = 0, binding = 4) writeonly buffer PlanEntries { uint w[]; } planEntries;
layout(std430, set = 0, binding = 5) buffer PlanArgs { uint w[]; } planArgs;

// round: the search's round, or a sort's shift; offset, count, scratch: a
// scan's run, its length and where its blocks' sums go.
layout(push_constant) uniform PlanPush {
    uint round;
    uint offset;
    uint count;
    uint scratch;
} planPush;

// --- IEEE's rounding (the CPU's) ---
// The first guess refined once by its remainder (exact by fma), then
// whichever of it and its neighbours leaves the least remainder.
float fnext(float x, int k) { return uintBitsToFloat(uint(int(floatBitsToUint(x)) + k)); }
float fdiv(float a, float b) {
    precise float q = a / b;
    if (isinf(q) || isnan(q) || q == 0.0 || isinf(a) || isinf(b)) return q;
    precise float r = fma(-q, b, a);
    precise float q1 = fma(r, 1.0 / b, q);
    float best = q1;
    precise float bestR = abs(fma(-q1, b, a));
    for (int k = -1; k <= 1; k += 2) {
        float c = fnext(q1, k);
        precise float cr = abs(fma(-c, b, a));
        if (cr < bestR) { best = c; bestR = cr; }
    }
    return best;
}
// The correctly rounded root of hi + lo (lo under an ulp of hi).
float fsqrt2(float hi, float lo) {
    if (hi <= 0.0) return 0.0;
    if (isinf(hi) || isnan(hi)) return sqrt(hi);
    precise float s = sqrt(hi);
    precise float r = fma(-s, s, hi) + lo;
    precise float s1 = fma(r, 0.5 / s, s);
    float best = s1;
    precise float bestR = abs(fma(-s1, s1, hi) + lo);
    for (int k = -1; k <= 1; k += 2) {
        float c = fnext(s1, k);
        precise float cr = abs(fma(-c, c, hi) + lo);
        if (cr < bestR) { best = c; bestR = cr; }
    }
    return best;
}
float fsqrt(float x) { return fsqrt2(x, 0.0); }
// bromath::vlen: sqrtf of the float dot, summed x, y, z in turn.
float flen(vec3 v) {
    precise float d = v.x * v.x + v.y * v.y + v.z * v.z;
    return fsqrt(d);
}
// std::hypot(float, float): the root of the exact x^2 + y^2, rounded once
// (as the C libraries' float hypot, through double, gives it).
float fhypot(float x, float y) {
    precise float hx = x * x, lx = fma(x, x, -hx);
    precise float hy = y * y, ly = fma(y, y, -hy);
    precise float s = hx + hy;
    precise float bb = s - hx;
    precise float e = (hx - (s - bb)) + (hy - bb);
    precise float lo = e + (lx + ly);
    precise float hi = s + lo;
    precise float rest = lo - (hi - s);
    return fsqrt2(hi, rest);
}

// --- The drawer's constants (scene_mesh_drawer_forest.cpp; its comments say why) ---
const float kRunNear = 30.0;
const float kRunBand = 10.0;
const float kBloomMinPixels = 0.2;
const float kBarkCurvePixels = 6.0;
const float kBarkRibbonCut = 0.5;
const float kWoodShare = 0.6;
const float kWoodSeenRef = 0.13;
const float kLobeReach = 0.5;
const float kWoodShade = 0.5;
const float kTwigFinePixels = 120.0;
const float kTwigFineClumpLeaves = 40.0;
const float kTwigPixels = 0.15;
const float kTwigStemRadius = 0.04;
const float kTwigShootRadius = float(FOREST_SHOOT_RADIUS_BASE);
const float kTwigWidening0 = 0.95;
const float kTwigWidening1 = 1.3;

// The drawer's groups (forest::Group).
const int G_CORES = 0, G_LEAF_NEAR = 1, G_LEAF_CARD = 2, G_BARK20C = 3, G_BARK10C = 4, G_BARK20 = 5,
          G_BARK10 = 6, G_BARK5 = 7, G_TWIGS = 8, G_TWIGS_FINE = 9, G_BLOOMS = 10, G_CLUMPS = 11,
          G_CROWNS = 12, G_RUNS = 13;

// --- Reading the buffers ---
uint pu(uint i) { return planParams.w[i]; }
float pf(uint i) { return uintBitsToFloat(planParams.w[i]); }
vec3 pv3(uint i) { return vec3(pf(i), pf(i + 1u), pf(i + 2u)); }
vec4 pv4(uint i) { return vec4(pf(i), pf(i + 1u), pf(i + 2u), pf(i + 3u)); }
uint su(uint i) { return planStatic.w[i]; }
float sf(uint i) { return uintBitsToFloat(planStatic.w[i]); }
vec3 sv3(uint i) { return vec3(sf(i), sf(i + 1u), sf(i + 2u)); }
uint workRun(uint run) { return pu(FP_P_WORK + run); }
// The trees the measure stage measured this frame (FP_N_MEASURED).
uint planMeasured() { return planWork.w[FP_N_MEASURED]; }

// A scan of `count` words whose length the GPU sets: each level's count
// (the WORK words from `words`) and blocks to dispatch (the ARGS words from
// `args`, 4 a level), for GpuPlanner::scanSized's dispatches.
void planScanSized(uint count, uint words, uint args) {
    for (uint l = 0u; l < uint(FP_SCAN_LEVELS); ++l) {
        uint blocks = (count + uint(FP_SCAN_BLOCK) - 1u) / uint(FP_SCAN_BLOCK);
        planWork.w[words + l] = count;
        planArgs.w[args + 4u * l] = blocks;
        planArgs.w[args + 4u * l + 1u] = 1u;
        planArgs.w[args + 4u * l + 2u] = 1u;
        count = blocks;
    }
}

// A 64-bit sum in two words, added to by many threads: exact in any order.
void workAdd64(uint at, uint v) {
    if (v == 0u) return;
    uint old = atomicAdd(planWork.w[at], v);
    if (old + v < old) atomicAdd(planWork.w[at + 1u], 1u);
}
void sharedAdd64(uint at, uint v) {
    if (v == 0u) return;
    uint old = atomicAdd(planShared.w[at], v);
    if (old + v < old) atomicAdd(planShared.w[at + 1u], 1u);
}

struct Lod {
    float leafPixels, cardWidening, clumpFrom, clumpTo, crownFrom, crownTo, barkPixels, barkSidesPixels;
    float shadowKeep, range, barkKeepPixels, barkFineSides, leavesRange, leavesBand, detail;
};
Lod planLod(uint at) {
    Lod l;
    l.leafPixels = pf(at + FP_LOD_LEAF_PIXELS);
    l.cardWidening = pf(at + FP_LOD_CARD_WIDENING);
    l.clumpFrom = pf(at + FP_LOD_CLUMP_FROM);
    l.clumpTo = pf(at + FP_LOD_CLUMP_TO);
    l.crownFrom = pf(at + FP_LOD_CROWN_FROM);
    l.crownTo = pf(at + FP_LOD_CROWN_TO);
    l.barkPixels = pf(at + FP_LOD_BARK_PIXELS);
    l.barkSidesPixels = pf(at + FP_LOD_BARK_SIDES_PIXELS);
    l.shadowKeep = pf(at + FP_LOD_SHADOW_KEEP);
    l.range = pf(at + FP_LOD_RANGE);
    l.barkKeepPixels = pf(at + FP_LOD_BARK_KEEP_PIXELS);
    l.barkFineSides = pf(at + FP_LOD_BARK_FINE_SIDES);
    l.leavesRange = pf(at + FP_LOD_LEAVES_RANGE);
    l.leavesBand = pf(at + FP_LOD_LEAVES_BAND);
    l.detail = 1.0;
    return l;
}
// ForestNode::scaled.
Lod lodScaled(Lod l, float d) {
    precise float leaf = l.leafPixels * d, bark = l.barkPixels * d, keep = l.barkKeepPixels * d;
    l.leafPixels = leaf;
    l.barkPixels = bark;
    l.detail = d;
    l.barkKeepPixels = keep;
    return l;
}

struct Spec {
    uint segStart, segCount, clumpStart, clumpCount, leafStart, leafCount, bloomStart, bloomCount;
    vec3 crown;
    float crownRadius, crownRadiusUp, meanClumpRadius, trunkRadius, bloomExtent, woodSeen;
    bool pinnate;
    vec3 barkOverLeaf;
};
Spec planSpec(uint k) {
    uint b = su(FP_ST_SPECIMENS) + k * FP_SPEC_WORDS;
    Spec s;
    s.segStart = su(b + FP_SPEC_SEG_START);
    s.segCount = su(b + FP_SPEC_SEG_COUNT);
    s.clumpStart = su(b + FP_SPEC_CLUMP_START);
    s.clumpCount = su(b + FP_SPEC_CLUMP_COUNT);
    s.leafStart = su(b + FP_SPEC_LEAF_START);
    s.leafCount = su(b + FP_SPEC_LEAF_COUNT);
    s.bloomStart = su(b + FP_SPEC_BLOOM_START);
    s.bloomCount = su(b + FP_SPEC_BLOOM_COUNT);
    s.crown = sv3(b + FP_SPEC_CROWN);
    s.crownRadius = sf(b + FP_SPEC_CROWN_RADIUS);
    s.crownRadiusUp = sf(b + FP_SPEC_CROWN_UP);
    s.meanClumpRadius = sf(b + FP_SPEC_CLUMP_RADIUS);
    s.trunkRadius = sf(b + FP_SPEC_TRUNK_RADIUS);
    s.bloomExtent = sf(b + FP_SPEC_BLOOM_EXTENT);
    s.pinnate = su(b + FP_SPEC_PINNATE) != 0u;
    s.woodSeen = sf(b + FP_SPEC_WOOD_SEEN);
    s.barkOverLeaf = sv3(b + FP_SPEC_BARK_OVER_LEAF);
    return s;
}

struct Tree {
    vec3 bmin, bmax;   // world, padded by the sway's reach (ForestCull::bounds, as uploaded)
    vec3 position;
    float scale, cosYaw, sinYaw;
    uint specimen;
};
Tree planTree(uint i) {
    uint b = su(FP_ST_TREES) + i * FP_TREE_WORDS;
    Tree t;
    t.bmin = sv3(b + FP_TREE_MIN);
    t.bmax = sv3(b + FP_TREE_MAX);
    t.position = sv3(b + FP_TREE_POSITION);
    t.scale = sf(b + FP_TREE_SCALE);
    t.cosYaw = sf(b + FP_TREE_COS_YAW);
    t.sinYaw = sf(b + FP_TREE_SIN_YAW);
    t.specimen = su(b + FP_TREE_SPECIMEN);
    return t;
}

// bromath::mtransformPoint with the rows at `at`.
vec3 rowsPoint(uint at, vec3 p) {
    vec3 o;
    for (uint r = 0u; r < 3u; ++r) {
        vec4 m = pv4(at + 4u * r);
        precise float v = m.x * p.x + m.y * p.y + m.z * p.z + m.w;
        o[r] = v;
    }
    return o;
}

// bromath::fintersects(frustum, box): no plane has the whole box behind it.
bool planIntersects(uint planes, vec3 bmin, vec3 bmax) {
    if (bmin.x > bmax.x || bmin.y > bmax.y || bmin.z > bmax.z) return false;
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = pv4(planes + 4u * i);
        vec3 v = vec3(p.x >= 0.0 ? bmax.x : bmin.x, p.y >= 0.0 ? bmax.y : bmin.y, p.z >= 0.0 ? bmax.z : bmin.z);
        precise float d = p.x * v.x + p.y * v.y + p.z * v.z + p.w;
        if (d < 0.0) return false;
    }
    return true;
}

// --- The view this stage plans for (planBegin) ---
vec3 gEye;
float gPxAtMetre, gNodeScale, gLeafLength;
bool gPerspective;
Lod gLod, gShadowLod;   // the camera's and the casters' (unscaled)
uint gLeafVerts, gLeafCopies, gCardVerts, gBloomBase, gTrees, gTiles;
bool gCores, gClusters, gTwigs, gNeedles;
float gDetail;   // the plan's detail (the twigs' fine rows and shoots)

void planBegin() {
    gEye = pv3(FP_P_EYE);
    gPxAtMetre = pf(FP_P_PX_AT_METRE);
    gNodeScale = pf(FP_P_NODE_SCALE);
    gLeafLength = pf(FP_P_LEAF_LENGTH);
    gPerspective = pu(FP_P_PERSPECTIVE) != 0u;
    gLod = planLod(FP_P_LOD);
    gShadowLod = planLod(FP_P_SHADOW_LOD);
    gLeafVerts = pu(FP_P_LEAF_VERTS);
    gLeafCopies = pu(FP_P_LEAF_COPIES);
    gCardVerts = pu(FP_P_CARD_VERTS);
    gBloomBase = pu(FP_P_BLOOM_BASE);
    gTrees = pu(FP_P_TREES);
    gTiles = pu(FP_P_TILES);
    uint f = pu(FP_P_FLAGS);
    gCores = (f & FP_F_CORES) != 0u;
    gClusters = (f & FP_F_CLUSTERS) != 0u;
    gTwigs = (f & FP_F_TWIGS) != 0u;
    gNeedles = (f & FP_F_NEEDLES) != 0u;
    gDetail = 1.0;
}

// The distance from the eye to a box's centre (the camera's order, a
// caster's distance).
float centreDist(Tree t) {
    precise vec3 c = (t.bmin + t.bmax) * 0.5 - gEye;
    return flen(c);
}
// A caster's pixels per specimen unit in tile `tile` at distance `dc`.
float tilePx(uint tile, Tree t, float dc) {
    uint b = FP_P_TILE + tile * FP_TILE_WORDS;
    precise float texel = pf(b + FP_TILE_TEXEL_CONST);
    if (pu(b + FP_TILE_ORTHO) == 0u) texel += pf(b + FP_TILE_TEXEL_PER_METRE) * dc;
    precise float unit = t.scale * gNodeScale;
    return fdiv(unit, max(texel, 1e-5));
}

// The camera's measure of a tree (FP_CAM_*).
struct Cam {
    bool seen;
    float px, barkPx, dist, far;
    uint curveEnd, fineEnd, nearEnd;
};
Cam camOf(uint tree) {
    uint b = workRun(FP_W_CAMERA) + tree * FP_CAM_WORDS;
    Cam c;
    c.seen = planWork.w[b + FP_CAM_SEEN] != 0u;
    c.px = uintBitsToFloat(planWork.w[b + FP_CAM_PX]);
    c.barkPx = uintBitsToFloat(planWork.w[b + FP_CAM_BARK_PX]);
    c.dist = uintBitsToFloat(planWork.w[b + FP_CAM_DIST]);
    c.far = uintBitsToFloat(planWork.w[b + FP_CAM_FAR]);
    c.curveEnd = planWork.w[b + FP_CAM_CURVE_END];
    c.fineEnd = planWork.w[b + FP_CAM_FINE_END];
    c.nearEnd = planWork.w[b + FP_CAM_NEAR_END];
    return c;
}

// --- The keep radii (thickest first) ---
float keepAt(uint i) { return sf(su(FP_ST_KEEP) + i); }
// std::upper_bound(k + from, k + to, v, r > v): the first segment from
// `from` thinner than `v` (k = the keep radii from `base`).
uint firstUnder(uint base, uint from, uint to, float v) {
    uint lo = from, hi = to;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1;
        if (v > keepAt(base + mid)) hi = mid; else lo = mid + 1u;
    }
    return lo;
}
// std::upper_bound(k + from, k + to, v, v >= r): the first at most `v` wide.
uint firstAtMost(uint base, uint from, uint to, float v) {
    uint lo = from, hi = to;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1;
        if (v >= keepAt(base + mid)) hi = mid; else lo = mid + 1u;
    }
    return lo;
}

// --- ForestNode::treeLod and the fades ---
struct TreeLod {
    float widening, keep, cardShare, leaves, clumps, crowns, barkRadius;
    int barkSides;
};

float planSmooth(float a, float b, float x) {
    if (a == b) return x < a ? 0.0 : 1.0;
    precise float xa = x - a, ba = b - a;
    precise float t = clamp(fdiv(xa, ba), 0.0, 1.0);
    precise float s = t * t * (3.0 - 2.0 * t);
    return s;
}

// (forceTier -1: the GPU path plans no forced tier.)
TreeLod planTreeLod(Lod lod, Spec s, float px, float leafLength, float barkPx, float dist, float distFar) {
    TreeLod t;
    t.widening = 1.0;
    t.keep = 1.0;
    t.cardShare = 0.0;
    px = max(px, 1e-6);
    barkPx = barkPx > 0.0 ? barkPx : px;
    precise float twoBark = 2.0 * barkPx;
    precise float barkRadius = fdiv(lod.barkPixels, twoBark);
    precise float trunkPx = 2.0 * s.trunkRadius * barkPx;
    precise float fineSides = lod.barkFineSides * lod.barkSidesPixels;
    t.barkSides = lod.barkFineSides > 0.0 && trunkPx > fineSides ? FOREST_BARK_SIDES_NEAREST
                : trunkPx > lod.barkSidesPixels ? FOREST_BARK_SIDES_NEAR : FOREST_BARK_SIDES_FAR;
    precise float leafPx = max(leafLength * px, 1e-6);
    precise float widening = max(1.0, fdiv(lod.leafPixels, leafPx));
    t.widening = widening;
    precise float ww = widening * widening;
    t.keep = fdiv(1.0, ww);
    barkRadius *= widening;
    if (lod.barkKeepPixels > 0.0) barkRadius = min(barkRadius, fdiv(lod.barkKeepPixels, twoBark));
    t.barkRadius = barkRadius;
    float c0 = fdiv(lod.cardWidening, 1.06);
    precise float c1 = lod.cardWidening * 1.06;
    t.cardShare = planSmooth(c0, c1, widening);
    float toClumps = planSmooth(lod.clumpFrom, lod.clumpTo, widening);
    precise float clumpPx = s.meanClumpRadius * px;
    precise float toCrowns = s.clumpCount > 0u ? 1.0 - planSmooth(lod.crownTo, lod.crownFrom, clumpPx) : 0.0;
    t.crowns = toCrowns;
    precise float clumps = toClumps * (1.0 - toCrowns);
    precise float leaves = (1.0 - toClumps) * (1.0 - toCrowns);
    t.clumps = s.clumpCount > 0u ? clumps : 0.0;
    t.leaves = s.clumpCount > 0u ? leaves : 1.0;
    if (lod.leavesRange > 0.0 && dist >= 0.0 && s.clumpCount > 0u) {
        precise float half_ = 0.5 * lod.leavesBand;
        float farD = max(distFar, dist);
        precise float inner = lod.leavesRange + half_;
        precise float outer = lod.leavesRange - half_;
        if (dist < inner) {
            t.leaves = 1.0;
            t.clumps = farD > outer ? 1.0 : 0.0;
            t.crowns = 0.0;
        } else {
            precise float sum = t.clumps + t.leaves;
            t.clumps = sum;
            t.leaves = 0.0;
        }
    }
    return t;
}

// std::round / std::lround for a value at least 0 (half away from zero).
float roundHalfUp(float x) {
    float r = floor(x);
    precise float f = x - r;
    return f >= 0.5 ? r + 1.0 : r;
}
// ForestNode::fadeCode.
float planFadeCode(float lo, float hi) {
    lo = clamp(lo, 0.0, 1.0);
    hi = clamp(hi, 0.0, 1.0);
    if (lo <= 0.0 && hi >= 1.0) return 1.0;
    precise float code = 2.0 * roundHalfUp(lo * 4096.0) + hi;
    return code;
}
// ForestNode::fadeRanges.
void planFadeRanges(TreeLod t, out float lo[3], out float hi[3]) {
    float w[3] = float[3](t.leaves, t.clumps, t.crowns);
    float at = 0.0;
    int last = -1;
    for (int j = 0; j < 3; ++j) {
        int k = 2 - j;
        lo[k] = at;
        precise float up = at + max(w[k], 0.0);
        hi[k] = k == 0 ? (w[0] > 0.0 ? 1.0 : at) : up;
        at = hi[k];
        if (w[k] > 0.0) last = k;
    }
    for (int k = 0; k < 3; ++k)
        if (w[k] >= 1.0) { lo[k] = 0.0; hi[k] = 1.0; }
    if (last >= 0 && w[last] > 0.0) hi[last] = 1.0;
    int drawn = 0;
    for (int k = 0; k < 3; ++k) drawn += w[k] > 0.0 ? 1 : 0;
    if (drawn == 1)
        for (int k = 0; k < 3; ++k)
            if (w[k] > 0.0) lo[k] = 0.0;
}

// --- ForestPlan::casterLod ---
TreeLod planCasterLod(Lod L, Spec sp, float px, float dist, uint tree) {
    Cam cam = camOf(tree);
    bool seenTree = cam.seen;
    float camDist = seenTree ? cam.dist : dist;
    float camFar = seenTree ? cam.far : dist;
    TreeLod t = planTreeLod(L, sp, px, gLeafLength, 0.0, camDist, camFar);
    precise float sphere = L.leavesRange + 0.5 * L.leavesBand;
    if (L.leavesRange > 0.0 && camDist >= 0.0 && camDist < sphere && sp.clumpCount > 0u) {
        float w = max(1.0, L.detail);
        t.widening = w;
        precise float ww = w * w;
        t.keep = fdiv(1.0, ww);
    }
    if (t.leaves >= 1.0 && t.clumps >= 1.0) return t;
    if (!seenTree) return t;
    TreeLod c = planTreeLod(gLod, sp, cam.px, gLeafLength, 0.0, camDist, camFar);
    precise float gap = c.leaves - t.leaves;
    float back = min(t.clumps, max(gap, 0.0));
    precise float clumpsLeft = t.clumps - back, leavesNow = t.leaves + back;
    t.clumps = clumpsLeft;
    t.leaves = leavesNow;
    if (t.crowns <= 0.0) return t;
    float cc = max(c.crowns, 0.0);
    precise float cube = cc * cc * cc;
    float keep = min(t.crowns, gNeedles ? cube : cc);
    precise float clumps = t.clumps + (t.crowns - keep);
    t.clumps = clumps;
    t.crowns = keep;
    return t;
}

// --- ForestPlan::clumpPx ---
float planClumpPx(Tree tr, uint c, out float radius) {
    radius = 0.0;
    if (c >= su(FP_ST_CLUMP_COUNT)) return 0.0;
    uint b = su(FP_ST_CLUMPS) + c * 4u;
    vec3 q = sv3(b);
    float qr = sf(b + 3u);
    precise vec3 local = vec3(q.x * tr.cosYaw + q.z * tr.sinYaw, q.y, -q.x * tr.sinYaw + q.z * tr.cosYaw);
    precise vec3 p = tr.position + local * tr.scale;
    vec3 w = rowsPoint(FP_P_WORLD, p);
    precise float unit = tr.scale * gNodeScale;
    precise vec3 toEye = w - gEye;
    precise float d = max(flen(toEye) - qr * unit, 0.25);
    radius = qr;
    precise float px = (gPerspective ? fdiv(gPxAtMetre, d) : gPxAtMetre) * unit;
    return px;
}

// --- ForestPlan::addTree ---
// MODE_COST: the vertices alone (addTree with no groups). MODE_COUNT: each
// group's entries and the posed groups' leaves (gCount), and the stats'
// tallies. MODE_EMIT: the entries, written at gAt (gSlot their pose slots).
// (Not PLAN_*: those name the stages' builds.)
const uint MODE_COST = 0u, MODE_COUNT = 1u, MODE_EMIT = 2u;
uint gMode;
uint gVerts, gDrawnOnly, gLeaves, gBlooms;
uint gCount[FP_COLUMNS];
uint gAt[FP_GROUPS];
uint gSlot[2];     // the block's view's leaf groups (LeafNear, LeafCard)
bool gPosed[2];
// View v's leaf group j (0 LeafNear, 1 LeafCard) as a posed group
// (FP_POSED_GROUPS), -1 for none: a tile poses its LeafCard casters alone.
int posedGroupOf(uint v, int j) { return v == 0u ? j : j == 1 ? int(1u + v) : -1; }
uint gEntryCap;
// A broad tree's twigs, a clump at a time, are the workgroup's when the
// stage defers them (gDeferTwigs; forest_plan_place.comp's twigs()): the
// tree's thread leaves its job here, its clumps and what they are measured
// against, and in MODE_EMIT moves its twig groups' places past them
// (gTwigTotal, the entries the count pass found). One thread looping a near
// tree's clumps held its workgroup: on foot the search took 0.63 GPU ms.
bool gDeferTwigs = false;
struct TwigJob {
    bool on;
    uint first, count;    // its clumps
    bool shoots, mayFine;
    float shootMin, fineMin;
    float widening, keep, fade;
    uint base[2];         // MODE_EMIT: its first plain and fine twig entries
};
TwigJob gTwigJob;
uint gTwigTotal[2];

// A deferred twig job's clump `c` (addTree's twig loop, one turn of it): its
// pieces and whether they are drawn fine.
void twigClump(TwigJob j, Tree tr, uint c, out uint n, out bool fine) {
    float radius = 0.0;
    float cpx = j.shoots || j.mayFine ? planClumpPx(tr, c, radius) : 0.0;
    precise float shootC = 2.0 * kTwigShootRadius * gLeafLength * cpx;
    bool shootsC = j.shoots && shootC >= j.shootMin;
    uint twigs = su(FP_ST_TWIGS);
    n = shootsC ? su(twigs + 2u * c + 1u) : su(twigs + 2u * c);
    precise float clumpW = radius * cpx;
    fine = j.mayFine && clumpW >= j.fineMin;
}

void planReset() {
    gVerts = 0u;
    gDrawnOnly = 0u;
    gLeaves = 0u;
    gBlooms = 0u;
    for (int k = 0; k < FP_COLUMNS; ++k) gCount[k] = 0u;
    gTwigJob.on = false;
    gTwigJob.count = 0u;
}

// An entry written at `at` (past the room: dropped, its count still asked).
void entryAt(uint at, uint inst, uint first, uint end, uint param, float widening, float keepEnd, float tail,
             float fade, vec3 wood, uint slot) {
    if (at >= gEntryCap) return;
    uint b = at * uint(FOREST_ENTRY_RECORDS * 4);
    planEntries.w[b + FOREST_E_INSTANCE] = inst;
    planEntries.w[b + FOREST_E_FIRST] = first;
    planEntries.w[b + FOREST_E_END] = end;
    planEntries.w[b + FOREST_E_PARAM] = param;
    planEntries.w[b + FOREST_E_WIDENING] = floatBitsToUint(widening);
    planEntries.w[b + FOREST_E_KEEP_END] = floatBitsToUint(keepEnd);
    planEntries.w[b + FOREST_E_TAIL] = floatBitsToUint(tail);
    planEntries.w[b + FOREST_E_FADE] = floatBitsToUint(fade);
    planEntries.w[b + FOREST_E_SLOT] = slot;
    planEntries.w[b + FOREST_E_WOOD] = floatBitsToUint(wood.x);
    planEntries.w[b + FOREST_E_WOOD + 1] = floatBitsToUint(wood.y);
    planEntries.w[b + FOREST_E_WOOD + 2] = floatBitsToUint(wood.z);
}
void planEntry(int k, uint inst, uint first, uint end, uint param, float widening, float keepEnd, float tail,
               float fade, vec3 wood, bool camera) {
    // A leaf group that may be posed: the camera's two, a tile's casters.
    bool leaf = k == G_LEAF_CARD || (camera && k == G_LEAF_NEAR);
    if (gMode == MODE_COUNT) {
        gCount[k]++;
        if (leaf) gCount[FP_GROUPS + k - G_LEAF_NEAR] += end - first;
        return;
    }
    uint at = gAt[k]++;
    uint slot = 0xffffffffu;
    if (leaf) {
        int j = k - G_LEAF_NEAR;
        if (gPosed[j]) slot = gSlot[j];
        gSlot[j] += end - first;
    }
    entryAt(at, inst, first, end, param, widening, keepEnd, tail, fade, wood, slot);
}
// A run [0, n) in entries of `per` each, from `base`: counted at once, or written.
void planRun(int k, uint inst, uint base, uint n, uint per, uint param, float widening, float keepEnd, float tail,
             float fade, vec3 wood, bool camera) {
    if (n == 0u) return;
    if (gMode == MODE_COUNT) {
        gCount[k] += (n + per - 1u) / per;
        if (k == G_LEAF_CARD || (camera && k == G_LEAF_NEAR)) gCount[FP_GROUPS + k - G_LEAF_NEAR] += n;
        return;
    }
    for (uint c = 0u; c < n; c += per)
        planEntry(k, inst, base + c, base + min(c + per, n), param, widening, keepEnd, tail, fade, wood, camera);
}
void planDrawn(uint n) {
    gVerts += n;
    gDrawnOnly += n;
}
uint entriesOf(uint m, uint per) { return (m + per - 1u) / per; }
uint barkRows(int sides) { return sides >= 20 ? 3u : sides >= 10 ? 2u : 1u; }

void planAddTree(uint inst, Tree tr, Spec sp, TreeLod t, bool shadow, float px, float barkPx, bool own, Cam sb,
                 float castDist) {
    bool G = gMode != MODE_COST;
    uint keepBase = sp.segStart;
    vec3 wood = vec3(0.0);
    float lobeReach = 1.0;
    if (G && !shadow && sp.segCount > 0u && t.barkRadius > 0.0 && sp.crownRadius > 0.0) {
        precise float cutAt = kBarkRibbonCut * t.barkRadius;
        uint cut = firstUnder(keepBase, 0u, sp.segCount, cutAt);
        uint areas = su(FP_ST_AREA);
        precise float area = sf(areas + sp.segStart + sp.segCount) - sf(areas + sp.segStart + cut);
        precise float crown = 3.14159265 * sp.crownRadius * max(sp.crownRadiusUp, sp.crownRadius * 0.25);
        float share = fdiv(area, crown);
        if (gTwigs) {
            precise float reach = 1.0 + clamp(kLobeReach * share, 0.0, 0.3);
            lobeReach = reach;
        }
        float seen = gTwigs ? 1.0 : fdiv(sp.woodSeen, kWoodSeenRef);
        precise float fRaw = kWoodShare * share * seen;
        float f = clamp(fRaw, 0.0, 0.5);
        if (f > 0.0) {
            float shade = gTwigs ? kWoodShade : 1.0;
            for (int i = 0; i < 3; ++i) {
                precise float w = 1.0 + f * (shade * sp.barkOverLeaf[i] - 1.0);
                wood[i] = w;
            }
        }
    }
    vec3 leafWood = gTwigs ? vec3(0.0) : wood;
    float lo[3], hi[3];
    planFadeRanges(t, lo, hi);
    if (shadow) {
        float w3[3] = float[3](t.leaves, t.clumps, t.crowns);
        for (int k = 0; k < 3; ++k) { lo[k] = 0.0; hi[k] = clamp(w3[k], 0.0, 1.0); }
    }
    // Leaves.
    if (hi[0] > lo[0] && sp.leafCount > 0u) {
        float cardShare = shadow ? 1.0 : clamp(t.cardShare, 0.0, 1.0);
        float widening = t.widening;
        if (shadow) widening = fdiv(widening, fsqrt(max(gLod.shadowKeep, 0.01)));
        precise float ww = widening * widening;
        precise float keepF = fdiv(float(sp.leafCount), ww) * (shadow ? hi[0] : 1.0);
        uint n = min(sp.leafCount, uint(ceil(keepF)));
        precise float quarter = 0.25 * keepF;
        float tail = max(1.0, quarter);
        precise float keepEnd = float(sp.leafStart) + keepF;
        precise float nearF = float(n) * (1.0 - cardShare);
        uint nearN = min(n, uint(roundHalfUp(nearF)));
        for (int form = 0; form < 2; ++form) {
            bool card = form == 1;
            uint from = card ? nearN : 0u, to = card ? n : nearN;
            if (to <= from) continue;
            uint K = card ? uint(FOREST_CARD_COPIES) : gLeafCopies;
            bool cluster = card && !shadow && gClusters;
            uint V = !card ? gLeafVerts : cluster ? 6u : gCardVerts;
            gVerts += ((to - from + K - 1u) / K) * K * V;
            if (G) {
                float fade = planFadeCode(lo[0], hi[0]);
                planRun(card ? G_LEAF_CARD : G_LEAF_NEAR, inst, sp.leafStart + from, to - from, K, V, widening,
                        keepEnd, tail, fade, leafWood, !shadow);
                if (!shadow) gLeaves += to - from;
            }
        }
        // A broad node's twigs.
        if (!shadow && gTwigs && sp.clumpCount > 0u) {
            precise float stemPx = 2.0 * kTwigStemRadius * gLeafLength * px;
            precise float shootPx = 2.0 * kTwigShootRadius * gLeafLength * px;
            precise float w0 = gLod.cardWidening * kTwigWidening0, w1 = gLod.cardWidening * kTwigWidening1;
            precise float leafPx = gLeafLength * px;
            float bySize = fdiv(gLod.leafPixels, max(leafPx, 1e-6));
            float wide = max(t.widening, bySize);
            precise float span = w1 - w0, over = wide - w0;
            float q = clamp(fdiv(over, max(span, 1e-3)), 0.0, 1.0);
            precise float keep = 1.0 - q * q * (3.0 - 2.0 * q);
            if (stemPx >= kTwigPixels && keep > 0.0) {
                bool shoots = shootPx >= kTwigPixels;
                precise float fineMin = kTwigFinePixels * gDetail * gDetail;
                precise float fineAt = kTwigFineClumpLeaves * gLeafLength * px;
                bool mayFine = fineAt >= fineMin;
                float fade = planFadeCode(lo[0], hi[0]);
                precise float shootMin = kTwigPixels * gDetail * gDetail;
                uint twigs = su(FP_ST_TWIGS), twigCount = su(FP_ST_TWIG_COUNT);
                if (gDeferTwigs) {
                    uint end = min(sp.clumpStart + sp.clumpCount, twigCount);
                    gTwigJob.on = end > sp.clumpStart;
                    gTwigJob.first = sp.clumpStart;
                    gTwigJob.count = gTwigJob.on ? end - sp.clumpStart : 0u;
                    gTwigJob.shoots = shoots;
                    gTwigJob.mayFine = mayFine;
                    gTwigJob.shootMin = shootMin;
                    gTwigJob.fineMin = fineMin;
                    gTwigJob.widening = t.widening;
                    gTwigJob.keep = keep;
                    gTwigJob.fade = fade;
                    if (gMode == MODE_EMIT) {
                        gTwigJob.base[0] = gAt[G_TWIGS];
                        gTwigJob.base[1] = gAt[G_TWIGS_FINE];
                        gAt[G_TWIGS] += gTwigTotal[0];
                        gAt[G_TWIGS_FINE] += gTwigTotal[1];
                    }
                }
                for (uint c = sp.clumpStart; !gDeferTwigs && c < sp.clumpStart + sp.clumpCount && c < twigCount; ++c) {
                    float radius = 0.0;
                    float cpx = shoots || mayFine ? planClumpPx(tr, c, radius) : 0.0;
                    precise float shootC = 2.0 * kTwigShootRadius * gLeafLength * cpx;
                    bool shootsC = shoots && shootC >= shootMin;
                    uint n = shootsC ? su(twigs + 2u * c + 1u) : su(twigs + 2u * c);
                    precise float clumpW = radius * cpx;
                    bool fine = mayFine && clumpW >= fineMin;
                    gVerts += entriesOf(n, FOREST_TWIG_PIECES_PER_ENTRY) * uint(FOREST_TWIG_PIECES_PER_ENTRY) *
                              uint(FOREST_TWIG_VERTICES) * (fine ? uint(FOREST_TWIG_FINE_ROWS) : 1u);
                    if (!G) continue;
                    planRun(fine ? G_TWIGS_FINE : G_TWIGS, inst, 0u, n, FOREST_TWIG_PIECES_PER_ENTRY, c, t.widening,
                            keep, 1.0, fade, vec3(0.0), false);
                }
            }
        }
        // A spray node's lobe cores.
        if (G && !shadow && gCores && sp.clumpCount > 0u) {
            float fade = planFadeCode(lo[0], hi[0]);
            planDrawn(entriesOf(sp.clumpCount, FOREST_CLUMPS_PER_ENTRY) * uint(FOREST_CLUMPS_PER_ENTRY) *
                      uint(FOREST_CLUMP_VERTICES));
            planRun(G_CORES, inst, sp.clumpStart, sp.clumpCount, FOREST_CLUMPS_PER_ENTRY, 0u, 1.0, 0.0, 1.0, fade,
                    vec3(0.0), false);
        }
    }
    // Bark.
    if (sp.segCount > 0u) {
        uint n = firstUnder(keepBase, 0u, sp.segCount, t.barkRadius);
        int sides = shadow ? FOREST_BARK_SIDES_FAR : t.barkSides;
        uint rows = barkRows(sides);
        bool ownEnds = own && sb.curveEnd != FP_ALL_SEGMENTS;
        uint nc = 0u;
        if (rows > 1u && px > 0.0)
            nc = ownEnds ? min(sb.curveEnd, n) : firstUnder(keepBase, 0u, n, fdiv(kBarkCurvePixels, px));
        precise float wpx = 2.0 * 1.25 * max(px, barkPx);
        precise float fine = gLod.barkFineSides * gLod.barkSidesPixels;
        uint c20 = 0u;
        if (sides == FOREST_BARK_SIDES_NEAREST)
            c20 = ownEnds ? clamp(sb.fineEnd, 0u, nc) : (wpx > 0.0 ? firstAtMost(keepBase, 0u, nc, fdiv(fine, wpx)) : nc);
        uint n20 = n, n10 = n;
        if (sides > FOREST_BARK_SIDES_FAR) {
            n10 = ownEnds ? clamp(sb.nearEnd, nc, n)
                          : (wpx > 0.0 ? firstAtMost(keepBase, nc, n, fdiv(gLod.barkSidesPixels, wpx)) : n);
            if (sides == FOREST_BARK_SIDES_NEAREST) {
                uint f20 = ownEnds ? clamp(sb.fineEnd, nc, n)
                                   : (wpx > 0.0 ? firstAtMost(keepBase, nc, n, fdiv(fine, wpx)) : n);
                n20 = min(f20, n10);
            } else {
                n20 = nc;
            }
        }
        const uint perEntry = uint(FOREST_SEGMENTS_PER_ENTRY) * 6u;
        gVerts += entriesOf(c20, FOREST_SEGMENTS_PER_ENTRY) * 3u * 20u * perEntry;
        gVerts += entriesOf(nc - c20, FOREST_SEGMENTS_PER_ENTRY) * 2u * 10u * perEntry;
        gVerts += entriesOf(n20 - nc, FOREST_SEGMENTS_PER_ENTRY) * uint(sides) * perEntry;
        gVerts += entriesOf(n10 - n20, FOREST_SEGMENTS_PER_ENTRY) * 10u * perEntry;
        gVerts += entriesOf(n - n10, FOREST_SEGMENTS_PER_ENTRY) * 5u * perEntry;
        if (G) {
            const uint rowsShift = uint(FOREST_BARK_ROWS_SHIFT);
            uint s0 = sp.segStart;
            planRun(G_BARK20C, inst, s0, c20, FOREST_SEGMENTS_PER_ENTRY, 20u | 3u << rowsShift, t.barkRadius, 0.0,
                    1.0, 1.0, vec3(0.0), false);
            planRun(G_BARK10C, inst, s0 + c20, nc - c20, FOREST_SEGMENTS_PER_ENTRY, 10u | 2u << rowsShift,
                    t.barkRadius, 0.0, 1.0, 1.0, vec3(0.0), false);
            int k = sides == FOREST_BARK_SIDES_NEAREST ? G_BARK20 : sides == FOREST_BARK_SIDES_NEAR ? G_BARK10 : G_BARK5;
            planRun(k, inst, s0 + nc, n20 - nc, FOREST_SEGMENTS_PER_ENTRY, uint(sides) | 1u << rowsShift,
                    t.barkRadius, 0.0, 1.0, 1.0, vec3(0.0), false);
            planRun(G_BARK10, inst, s0 + n20, n10 - n20, FOREST_SEGMENTS_PER_ENTRY, 10u | 1u << rowsShift,
                    t.barkRadius, 0.0, 1.0, 1.0, vec3(0.0), false);
            planRun(G_BARK5, inst, s0 + n10, n - n10, FOREST_SEGMENTS_PER_ENTRY, 5u | 1u << rowsShift,
                    t.barkRadius, 0.0, 1.0, 1.0, vec3(0.0), false);
        }
        // The ribbons for what the round bark's cut leaves out.
        if (!shadow && t.barkRadius > 0.0 && G) {
            precise float a0 = 2.0 * t.barkRadius, a1 = kBarkRibbonCut * t.barkRadius;
            uint r0 = firstUnder(keepBase, 0u, sp.segCount, a0), r1 = firstUnder(keepBase, 0u, sp.segCount, a1);
            if (r1 > r0) {
                planDrawn(entriesOf(r1 - r0, FOREST_TWIG_PIECES_PER_ENTRY) * uint(FOREST_TWIG_PIECES_PER_ENTRY) *
                          uint(FOREST_TWIG_VERTICES));
                planRun(G_TWIGS, inst, sp.segStart + r0, r1 - r0, FOREST_TWIG_PIECES_PER_ENTRY, 0xffffffffu,
                        t.barkRadius, 1.0, kBarkRibbonCut, 1.0, vec3(0.0), false);
            }
        }
    }
    // Sprites: a caster's clumps tier split between its lobes and its runs.
    float mid = hi[1];
    if (shadow && !sp.pinnate) {
        precise float near_ = castDist - kRunNear;
        float w = clamp(fdiv(near_, kRunBand), 0.0, 1.0);
        precise float m = hi[1] - (hi[1] - lo[1]) * w;
        mid = m;
    }
    if (hi[1] > mid && sp.clumpCount > 0u && G) {
        uint n = (sp.clumpCount + uint(FOREST_CLUMP_BLOCK) - 1u) / uint(FOREST_CLUMP_BLOCK);
        planDrawn(entriesOf(n, FOREST_RUNS_PER_ENTRY) * uint(FOREST_RUNS_PER_ENTRY) * uint(FOREST_CARD_VERTICES));
        planRun(G_RUNS, inst, 0u, n, FOREST_RUNS_PER_ENTRY, 0u, 1.0, 0.0, 1.0, planFadeCode(mid, hi[1]), vec3(0.0),
                false);
    }
    if (mid > lo[1] && sp.clumpCount > 0u && G) {
        planDrawn(entriesOf(sp.clumpCount, FOREST_CLUMPS_PER_ENTRY) * uint(FOREST_CLUMPS_PER_ENTRY) *
                  uint(FOREST_CLUMP_VERTICES));
        planRun(G_CLUMPS, inst, sp.clumpStart, sp.clumpCount, FOREST_CLUMPS_PER_ENTRY, 0u, lobeReach, 0.0, 1.0,
                planFadeCode(lo[1], mid), wood, false);
    }
    if (hi[2] > lo[2] && sp.clumpCount > 0u && G) {
        planDrawn(6u);
        planEntry(G_CROWNS, inst, 0u, 1u, 0u, 1.0, 0.0, 1.0, planFadeCode(lo[2], hi[2]), vec3(0.0), false);
    }
    // Blooms.
    precise float bloomPx = sp.bloomExtent * px;
    if (G && !shadow && sp.bloomCount > 0u && bloomPx >= kBloomMinPixels) {
        planDrawn(entriesOf(sp.bloomCount, FOREST_BLOOMS_PER_ENTRY) * uint(FOREST_BLOOMS_PER_ENTRY) *
                  uint(FOREST_CARD_VERTICES));
        planRun(G_BLOOMS, inst, sp.bloomStart, sp.bloomCount, FOREST_BLOOMS_PER_ENTRY, gBloomBase, 1.0, 0.0, 1.0,
                1.0, vec3(0.0), false);
        gBlooms += sp.bloomCount;
    }
}

// An item (a tree of a view): its tree and view, and what the plan measures
// it by. The camera's from its measure; a caster's from its tile.
struct Item {
    bool valid;
    uint tree;
    uint view;   // 0 the camera, 1 + tile
    Tree tr;
    Spec sp;
    float px, barkPx, dist, far;
    Cam cam;
};
Item planItem(uint item) {
    Item it;
    it.valid = item != FP_NO_ITEM;
    it.tree = 0u;
    it.view = 0u;
    if (!it.valid) return it;
    it.tree = item & FP_TREE_MASK;
    it.view = item >> FP_VIEW_SHIFT;
    it.tr = planTree(it.tree);
    it.sp = planSpec(it.tr.specimen);
    it.cam = camOf(it.tree);
    if (it.view == 0u) {
        it.px = it.cam.px;
        it.barkPx = it.cam.barkPx;
        it.dist = it.cam.dist;
        it.far = it.cam.far;
    } else {
        it.dist = centreDist(it.tr);
        it.px = tilePx(it.view - 1u, it.tr, it.dist);
        it.barkPx = 0.0;
        it.far = -1.0;
    }
    return it;
}
// ForestPlan::treeCost at detail `d`: the tree's tiers, then addTree in the
// mode set (gMode), from a reset tally.
TreeLod planItemAdd(Item it, float d) {
    bool shadow = it.view > 0u;
    gDetail = d;
    Lod L = lodScaled(shadow ? gShadowLod : gLod, d);
    TreeLod t = shadow ? planCasterLod(L, it.sp, it.px, it.dist, it.tree)
                       : planTreeLod(L, it.sp, it.px, gLeafLength, it.barkPx, it.dist, it.far);
    planReset();
    planAddTree(it.tree, it.tr, it.sp, t, shadow, it.px, it.barkPx, !shadow, it.cam, it.dist);
    return t;
}

// --- fitDetail's search, replayed from the sums of the asks so far ---
// The asks: 1, kMaxDetail, then 2 and 4 while they do not fit, then six
// bisections at sqrt(lo * hi) (the CPU's float roots, FP_G_TABLE: a node of
// each of the three trees, from (1, 2), (2, 4) or (4, 8), heap-numbered).
struct Search {
    bool done;
    float ask;      // the round's ask (not done)
    float result;   // the fit (done)
};
uvec2 searchSum(uint q, uint r) {
    uint at = FP_G_SUMS + (q * FP_ROUNDS + r) * 2u;
    uvec2 s = uvec2(planShared.w[at], planShared.w[at + 1u]);
    if (r >= 2u) {
        uint f = FP_G_FIXED + q * 2u;
        uint lo = s.x + planShared.w[f];
        s.y += planShared.w[f + 1u] + (lo < s.x ? 1u : 0u);
        s.x = lo;
    }
    return s;
}
bool searchOver(uint q, uint r) {
    uvec2 c = searchSum(q, r);
    uint b = FP_P_SEARCH + 4u * q;
    uvec2 limit = uvec2(pu(b + FP_Q_LIMIT), pu(b + FP_Q_LIMIT + 1u));
    return c.y > limit.y || (c.y == limit.y && c.x > limit.x);
}
// Search `q` (0 the camera's, 1 the shadow's) at round `round`: its ask
// there, or its fit when it asked no more before it.
Search planSearch(uint q, uint round) {
    Search s;
    s.done = true;
    s.ask = 1.0;
    s.result = 1.0;
    if (pu(FP_P_SEARCH + 4u * q + FP_Q_ON) == 0u) return s;
    s.done = false;
    uint r = 0u;
    if (round == r) { s.ask = 1.0; return s; }
    if (!searchOver(q, r)) { s.done = true; s.result = 1.0; return s; }
    r = 1u;
    if (round == r) { s.ask = float(FP_MAX_DETAIL); return s; }
    if (searchOver(q, r)) { s.done = true; s.result = float(FP_MAX_DETAIL); return s; }
    r = 2u;
    float lo = 1.0, hi = 2.0;
    uint tree = 0u;
    while (hi < float(FP_MAX_DETAIL)) {
        if (round == r) { s.ask = hi; return s; }
        bool over = searchOver(q, r);
        ++r;
        if (!over) break;
        lo = hi;
        hi *= 2.0;
        ++tree;
    }
    hi = min(hi, float(FP_MAX_DETAIL));
    tree = min(tree, 2u);
    uint node = 1u;
    for (int it = 0; it < 6; ++it) {
        float mid = uintBitsToFloat(planShared.w[FP_G_TABLE + tree * 64u + node]);
        if (round == r) { s.ask = mid; return s; }
        bool over = searchOver(q, r);
        ++r;
        if (over) { lo = mid; node = 2u * node + 1u; } else { hi = mid; node = 2u * node; }
    }
    s.done = true;
    s.result = hi;
    return s;
}
