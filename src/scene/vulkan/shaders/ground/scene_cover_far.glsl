// The cover's far tier: past the
// rings, what grows on the ground drawn as one small upright quad an
// element, as many of them as the screen holds rather than as the ground
// holds. Needs scene_camera.glsl and scene_grass.glsl.
//
// An element is a cell of the herbs' grid (grass.herbInfo.y m): the herb
// plant the module's herbStand puts there, or else a tuft of the sward where the grass
// grows. Its thinning is nested (stochastic simplification): cells of
// level k are 2^k herb cells a side, and each stands for its four children
// by one of them, chosen by a hash, down to a herb cell. A level-0 cell's
// rank is how many levels up it is the one chosen. At a point where the
// ground's image gives one herb cell `keep` of COVER_FAR_PIXELS squared,
// coverLevel's lambda is -log4(keep): the cells kept are those of rank at
// least floor(lambda) + 1, and of those of rank floor(lambda) a share that
// falls from all to none across the level, so 4^-lambda of them are kept,
// evenly (one a cell of the level, not in clumps), and as the eye recedes
// an element either stays (and widens, by 2^lambda, so the cover holds) or
// shrinks away; none appears from nowhere. The far herb ring keeps its
// plants by the same rule, so a plant there is the same one the far tier
// draws past it.
//
// The tier's plan enumerates each level's cells over the band of distance
// its lambda can fall in (scene_mesh_drawer_grass.cpp), and a cell's
// element is drawn by the level its lambda falls in.

// The ground's image a far element stands for (pixels, a side).
const float COVER_FAR_PIXELS = 5.0;
// The sine of the eye's line onto the ground no lower than this (so the
// thinning of ground seen edge on stays finite).
const float COVER_FAR_MIN_SIN = 0.01;
// The most a far element widens: past it the cover thins rather than its
// elements growing wider than a few metres (they stand as tall as they
// are, so seen across they overlap anyway).
const float COVER_FAR_WIDEN = 8.0;
// Its levels (scene_mesh_drawer_grass.cpp's kCoverLevels, a draw each; the
// level a draw's is grass.herbShape.x).
const int COVER_FAR_LEVELS = 10;

// Pixels a metre spans a metre from the eye.
float coverPixelsAtMetre() { return abs(camera.proj[1][1]) * camera.viewport.y * 0.5; }

// The thinning's level at ring distance `d` where the ground meets the
// eye's line at sine `sinE`.
float coverLevel(float d, float sinE) {
    float cellPx = grass.herbInfo.y * coverPixelsAtMetre() / max(d, 1e-3);
    float keep = cellPx * cellPx * max(sinE, COVER_FAR_MIN_SIN) / (COVER_FAR_PIXELS * COVER_FAR_PIXELS);
    return max(-0.5 * log2(max(keep, 1e-12)), 0.0);
}

// The child (a level k - 1 cell) that stands for level-k cell `P`.
ivec2 coverChild(ivec2 P, int k) {
    int b = int(grassHash(P, 9000 + k) * 4.0);
    return P * 2 + ivec2(b & 1, b >> 1);
}

// The herb cell level-`level` cell `C` stands for.
ivec2 coverRep(ivec2 C, int level) {
    for (int k = level; k > 0; --k) C = coverChild(C, k);
    return C;
}

// Whether herb cell `c`, of rank at least `k` (the rep of a level-k cell),
// is kept at lambda `lam` (floor(lam) == k): all of rank k + 1, of rank k a
// share falling across the level so that 4^-lam of the cells are kept. As
// the share falls to an element's own draw it shrinks away rather than
// going all at once: the size it keeps (0: not kept). It shrinks over a
// stretch of the share no wider than lies above its draw, so at the
// level's start (the share 1) every one is whole, as the level below left
// it.
float coverKeptAt(ivec2 c, ivec2 cellK, int k, float lam) {
    if (coverChild(cellK >> 1, k + 1) == cellK) return 1.0;
    float t = (exp2(2.0 * (1.0 - (lam - float(k)))) - 1.0) / 3.0;
    float h = grassHash(c, 9100);
    return h < t ? clamp((t - h) / (0.35 * (1.0 - h) + 1e-4), 0.0, 1.0) : 0.0;
}

// Whether herb cell `c` is kept at lambda `lam` (the far herb ring's test):
// the size it keeps, 0 when it is not.
float coverKept(ivec2 c, float lam) {
    int k = int(lam);
    ivec2 C = c;
    for (int j = 1; j <= k; ++j) {
        ivec2 P = C >> 1;
        if (coverChild(P, j) != C) return 0.0;
        C = P;
    }
    return coverKeptAt(c, C, k, lam);
}

// The widening of an element kept at lambda `lam`.
float coverWiden(float lam) { return min(exp2(lam), COVER_FAR_WIDEN); }

// A far element: a herb cell's plant (kind > 0: the module's variety kind - 1) or
// a tuft of the sward (kind 0).
struct CoverFar {
    vec4 h;        // the herb cell's hashes (scene_herb.glsl's herbPlantLive's)
    vec4 f;        // the field at its root
    vec3 gn;       // the ground's normal there
    vec2 root;     // absolute x, z
    float d;       // its ring distance
    float lam;     // its thinning's level
    float size;    // the share of its size it keeps (coverKeptAt)
    vec4 own;      // a tuft's: the sward module's (SwardCell.own, as swardDensity left it)
    int kind;
};

// A tuft's element as the sward module takes a cell (herb cell `c`, at its
// root: a tuft stands where its blades would, not gathered).
SwardCell coverFarSward(CoverFar e, ivec2 c) {
    SwardCell s;
    s.cell = c;
    s.h = e.h;
    s.root = e.root;
    s.f = e.f;
    s.ground = e.gn;
    s.d = e.d;
    s.kept = 1.0;
    s.own = e.own;
    return s;
}

// The hand-over from the rings: a herb is the far herb ring's (by the same
// dither as herbPlantLive's) inside its band; a tuft the far fans' as their
// ring ends (grass.ring: y its outer edge, w its band), by a dither of its
// own (the fans stand on cells of their own).
bool coverFarHanded(CoverFar e, ivec2 c) {
    if (e.kind > 0) return e.h.w > 1.0 - smoothstep(grass.herbInfo.z - grass.herbInfo.w, grass.herbInfo.z, e.d);
    return grassHash(c, 9201) > 1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, e.d);
}

// The element of level-`level` cell `C`, if the far tier draws it there:
// its lambda falls in the level (the last level takes every lambda past
// it), it is kept, past the rings, on the field and roughly on screen, and
// something grows there. `kind` -1 for none. With `known` >= 0 the kind is
// taken as known (the plan listed it): the vertices skip herbStand.
bool coverFarLive(ivec2 C, int level, int known, out CoverFar e) {
    ivec2 c = coverRep(C, level);
    e.h =vec4(grassHash(c, 101), grassHash(c, 102), grassHash(c, 103), grassHash(c, 104));
    e.f = vec4(0.0);
    e.gn = vec3(0.0, 1.0, 0.0);
    e.lam = 0.0;
    e.size = 1.0;
    e.own = vec4(0.0);
    e.kind = -1;
    e.root = (vec2(c) + e.h.xy) * grass.herbInfo.y;
    vec3 eye = coverEye();
    e.d = length(vec2(length(e.root - eye.xz), grass.lod.w));
    bool last = level == COVER_FAR_LEVELS - 1;
    // Its reach (grass.ring: x how far, z its last stretch, where it hands
    // over to the turf alone by a dither of its own).
    if (grassHash(c, 9204) > 1.0 - smoothstep(grass.ring.x - grass.ring.z, grass.ring.x, e.d)) return false;
    // (Its lambda is no lower than the ground seen face on gives: a level
    // that fine is past it, before the field is read.)
    if (int(coverLevel(e.d, 1.0)) > level) return false;
    if (!grassInField(e.root) || e.h.w > grassFieldInside(e.root)) return false;
    if (!coverOwns(e.root, grassHash(c, 8191))) return false;
    vec2 under;
    vec3 ground;
    grassGround(e.root, e.f, under, ground);
    vec4 cp = camera.viewProj * vec4(coverToEye(vec3(e.root.x - eye.x, e.f.r + 0.5 - eye.y, e.root.y - eye.z)), 1.0);
    float m = cp.w * 0.15 + 4.0;
    if (cp.w < -4.0 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;
    e.gn = ground;
    vec3 toEye = normalize(vec3(eye.x - e.root.x, eye.y - e.f.r, eye.z - e.root.y));
    float sinE = dot(e.gn, toEye);
    if (sinE <= 0.0) return false;
    e.lam = coverLevel(e.d, sinE);
    int k = int(e.lam);
    if (k < level || (k > level && !last)) return false;
    // (Past the last level its cells all stay.)
    if (k == level) {
        e.size = coverKeptAt(c, C, level, e.lam);
        if (e.size <= 0.0) return false;
    }

    if (known >= 0) {
        e.kind = known;
    } else {
        int v = grass.herbInfo.x > 0.5 ? herbStand(c, e.root, under) : -1;
        e.kind = v >= 0 && v < HERB_VARIETIES ? v + 1 : 0;
    }
    if (e.kind == 0) {
        // The sward's tufts, as thick as its blades stand there (the
        // module's swardDensity).
        SwardCell s = coverFarSward(e, c);
        float dens = e.f.a > 0.0 ? swardDensity(s) : 0.0;
        e.own = s.own;
        if (grassHash(c, 9203) > dens) {
            e.kind = -1;
            return false;
        }
    }
    return coverFarHanded(e, c);
}
