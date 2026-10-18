// A broad forest's leaves on their twigs (scene_forest.glsl's leaf form 0):
// every leaf hangs from a node of a current-year SHOOT, the shoots branch
// off a SPRAY's axis, each spray's axis is reached by a twig from its
// LOBE's stem, and the stems run from the clump's base, the end of the
// branch segment the clump grows on. All of it is derived from the clump's
// own record (centre, radius, twig direction, leaf count), its placement-key
// base (clump vegetation record r2.z, ForestNode::gpuData) and the
// specimen's leaf (its petiole, its phyllotaxis): nothing is stored per
// leaf or per twig, so every tree of every specimen can differ.
// broworkshop's lib/kit/sway.js ports it step for step (the motion
// oracle's ground truth); keep the two in step.
//
// A clump of n leaves carries S = max(1, (n + 4) / 8) shoots of about
// FOREST_SHOOT_LEAVES; NB sprays carry them (each about five shoots: a
// terminal one continuing its axis, the rest laterals alternating left and
// right), spread over the clump's three lobes (procClumpLobe) as a
// Fibonacci lattice over each lobe's sphere. A spray's axis and its shoots
// run along great circles of the sphere a little under the lobe's surface
// (its shell), so the foliage lies in a layer on the outer shell of the
// crown, flattened as the clump is (procClumpFlat). Leaf j (its key less
// the base) is on shoot j % S, its (j / S)'th leaf from the base: a node of
// the species' phyllotaxis (alternate in two ranks, opposite and decussate,
// or a 2/5 spiral), the leaves larger toward the middle and tip of the
// shoot. A leaf's blade leaves its node at FOREST_LEAF_ANGLE from the
// shoot, is turned toward the light (flattened into the spray's plane,
// facing out of the crown and up by the scatter's up bias) and droops.
//
// Wood is not straight: every twig is a curve through a few PIECES' joints
// that step aside, leaving and reaching each joint along the twig's way
// there (forestHermite), so its way runs on unbroken through every joint. A
// lobe's stem and a spray's twig from it are crooked (each joint a step of
// FOREST_CROOK of its piece's length, any way across it); a spray's axis
// and a shoot zigzag (each joint a step to alternate sides in the plane its
// leaves spread in, FOREST_ZIGZAG leaf lengths: more for alternate leaves,
// whose shoots zigzag node to node). A twig leaves the wood it grows from
// halfway between that wood's way and its own, sweeping out of it. What
// hangs from a twig hangs from the curve as drawn: a spray's twig leaves
// its lobe's stem, a shoot its spray's axis and a leaf its shoot at the
// point of the curve, so they meet. A twig's radius tapers from the stem to
// the shoot's tip, thicker where it carries more (forestPipe), and each
// stem, limb, axis and shoot narrows over its last piece to a point
// (FOREST_TWIG_TIP).
//
// The twigs (mesh_forest_tube.vert's twig build) are the same curves
// drawn as camera-facing ribbons, a piece at a time: FOREST_STEM_PIECES a
// lobe's stem, FOREST_SPRAY_PIECES a spray (its twig from the stem, then
// its axis), FOREST_SHOOT_PIECES a shoot. A twig's point and a leaf's node
// go through the same functions, so the two meet exactly, and move
// together: both take the wood's pose (vegPose) of the clump's record where
// they are, then the spray's and the shoot's own bending (forestShootSway),
// which only adds to the motion the leaf's flutter about its petiole rides
// on.
//
// Needs scene_procedural.glsl, scene_vegetation.glsl (sceneWindCycles) and
// scene_forest.glsl.

const int FOREST_SHOOT_LEAVES = 8;
const int FOREST_SPRAY_SHOOTS = 5;
const float FOREST_SHOOT_INTERNODE = 0.35;   // leaf lengths between a shoot's nodes
const float FOREST_SPRAY_LATERAL = 0.75;     // leaf lengths along a spray's axis between its shoots
const float FOREST_SHOOT_ANGLE = 1.0;        // radians a lateral shoot leaves its axis at
const float FOREST_LEAF_ANGLE = 0.95;        // radians a blade leaves its shoot at
// How far a blade turns into its spray's plane (1 flat in it), how far it
// droops, and how much of the scatter's tilt jitter its normal keeps.
const float FOREST_LEAF_FLATTEN = 0.6;
const float FOREST_LEAF_DROOP = 0.12;
const float FOREST_LEAF_TILT = 1.0;

// The pieces of each twig, and so of a clump.
// A limb and a spray's twig to its limb can run most of a large lobe (a
// grove broadleaf's clumps are 1.6 to 2.7 m across): in three pieces each
// was a run of straight rods a third of a metre long, so they take five,
// and a limb carries two sprays, so its sprays' twigs to it are short.
const int FOREST_STEM_PIECES = 4;     // a lobe's stem
const int FOREST_LIMB_PIECES = 5;     // a limb from it
const int FOREST_LIMB_SPRAYS = 2;     // sprays a limb carries (a lobe of 3 or more sprays has limbs)
const int FOREST_HUB_PIECES = 5;      // a spray's twig from its limb or its lobe's stem
const int FOREST_AXIS_PIECES = 4;     // a spray's axis
const int FOREST_SPRAY_PIECES = FOREST_HUB_PIECES + FOREST_AXIS_PIECES;
const int FOREST_SHOOT_PIECES = 4;
const int FOREST_STEMS = 3 * FOREST_STEM_PIECES;
// A crooked twig's joint steps this share of its piece's length aside; a
// zigzag's (a spray's axis, a shoot) this many leaf lengths, alternate
// leaves' (x) and the others' (y).
const float FOREST_CROOK = 0.2;
const vec2 FOREST_ZIGZAG = vec2(0.1, 0.045);
// A twig's radius in leaf lengths: a shoot's at its base and tip; wood
// carrying more shoots is thicker by their square root (the pipe model:
// its section is the sum of theirs), forestPipe.
const vec2 FOREST_SHOOT_RADIUS = vec2(0.011, 0.005);
float forestPipe(float shoots) { return FOREST_SHOOT_RADIUS.x * sqrt(max(shoots, 1.0)); }

// The shoots and sprays a clump of `n` leaves carries.
void forestShootCounts(int n, out int S, out int NB) {
    S = max(1, (n + 4) / FOREST_SHOOT_LEAVES);
    NB = S < 3 ? S : max(3, (S + 2) / FOREST_SPRAY_SHOOTS);
}

// The sprays lobe `lobe` of a clump of NB sprays carries (spray b is on
// lobe b % 3), and its limbs: none under 3 sprays (they leave the stem),
// else one for every FOREST_LIMB_SPRAYS. `forestLimbSlots` the most any of
// its lobes has (lobe 0's), each lobe's slots in the pieces.
int forestLobeSprays(int NB, int lobe) { return NB / 3 + (lobe < NB % 3 ? 1 : 0); }
int forestLobeLimbs(int L) { return L < 3 ? 0 : (L + FOREST_LIMB_SPRAYS - 1) / FOREST_LIMB_SPRAYS; }
int forestLimbSlots(int NB) { return forestLobeLimbs(forestLobeSprays(NB, 0)); }

// A clump's twig pieces: its lobe stems, limbs and sprays (`withShoots`
// false), or all of them. ForestNode::twigPieces counts the same.
int forestTwigPieces(int n, bool withShoots) {
    if (n <= 0) return 0;
    int S, NB;
    forestShootCounts(n, S, NB);
    return FOREST_STEMS + 3 * FOREST_LIMB_PIECES * forestLimbSlots(NB) + FOREST_SPRAY_PIECES * NB +
           (withShoots ? FOREST_SHOOT_PIECES * S : 0);
}

// What a clump's shoots are built from (specimen units).
struct ForestClumpShoots {
    vec3 C;        // the clump's centre
    float r;       // its radius
    vec3 D;        // its twig's direction (unit)
    vec3 B;        // its base: where it leaves its branch segment
    int n;         // its leaves
    int S, NB;     // its shoots and sprays
    int clump;     // its index (global)
    uint seed;     // the node's placement seed
    uint sb;       // its own stream
    float Lf;      // a leaf's length
    int phyllotaxis;   // 0 alternate (two ranks), 1 opposite (decussate), 2 spiral (2/5)
};

ForestClumpShoots forestClumpShoots(int clump, int phyllotaxis) {
    ForestClumpShoots c;
    vec4 r0 = procedural.records[forestClumpRecord(clump)];
    vec4 r1 = procedural.records[forestClumpRecord(clump) + 1];
    c.C = r0.xyz;
    c.r = r0.w;
    c.D = dot(r1.xyz, r1.xyz) > 1e-12 ? normalize(r1.xyz) : vec3(0.0, 1.0, 0.0);
    c.B = c.C - c.D * (0.35 * c.r);
    c.n = int(r1.w + 0.5);
    forestShootCounts(c.n, c.S, c.NB);
    c.clump = clump;
    c.seed = floatBitsToUint(procedural.header[0].x);
    c.sb = procHash(c.seed ^ procHash(uint(clump) * 0x632be5abu + 0x51ed27u) ^ 0x5eed1234u);
    c.Lf = max(procedural.header[1].z, 1e-4);
    c.phyllotaxis = phyllotaxis;
    return c;
}

// The clump's leaves are keyed from this (ForestNode::gpuData's r2.z).
int forestClumpKeyBase(int clump) {
    return int(floatBitsToUint(procedural.records[forestClumpRecord(clump) + 4].z));
}

// A point relative to its lobe, flattened as the clump is (clumpLeafFrame's).
vec3 forestShootFlat(vec3 v) {
    v.y *= (v.y < 0.0 ? 0.7 : 0.9) * procClumpFlat;
    return v;
}

// A joint's step aside of a crooked twig heading `T`: `amp` long (0.5 to
// 1 of it), any way across T, drawn from stream `h` at `k`.
vec3 forestCrook(uint h, int k, vec3 T, float amp) {
    vec3 a = perpendicularUnit(T);
    vec3 b = cross(T, a);
    float ang = PROC_TWO_PI * procDraw(h, uint(2 * k + 11));
    return (a * cos(ang) + b * sin(ang)) * (amp * (0.5 + 0.5 * procDraw(h, uint(2 * k + 12))));
}

// Where a twig of `K` pieces is at `u` (0..1 along it): its piece (k0),
// between its joints k0 and k0 + 1, and the share along it (f).
void forestPieceAt(float u, int K, out int k0, out float f) {
    float x = clamp(u, 0.0, 1.0) * float(K);
    k0 = min(int(floor(x)), K - 1);
    f = x - float(k0);
}

// Spray `b`: its lobe, its hub (where its twig leaves the lobe's stem), its
// axis' start on the lobe's shell (E, outward u, heading h, across sd), its
// length and the sphere its shoots follow (radius rho about the lobe's
// centre), its shoots.
struct ForestSpray {
    vec3 lc;
    float lr;
    vec3 hub;       // (forestSprayHub's: only its twig needs it)
    vec3 hubWay;    // its parent's way there (forestSprayHub's)
    vec3 u, h, sd;
    float rho, lam;
    int Q;          // its shoots
    uint hb;
    int lobe;
    float z;        // its place on the lobe's lattice: 1 facing out, -1 back toward the base
};

// The great circle of radius `rho` through `rho * u` heading `h`, `x` along
// it: its point (about the lobe's centre, unflattened), tangent and outward.
vec3 forestShootArc(vec3 u, vec3 h, float rho, float x, out vec3 T, out vec3 N) {
    float a = x / max(rho, 1e-5);
    float ca = cos(a), sa = sin(a);
    T = h * ca - u * sa;
    N = u * ca + h * sa;
    return N * rho;
}

vec3 forestQuadBezier(vec3 a, vec3 c, vec3 b, float t, out vec3 tangent) {
    vec3 d = 2.0 * mix(c - a, b - c, t);
    tangent = dot(d, d) > 1e-12 ? normalize(d) : normalize(b - a + vec3(1e-6));
    return mix(mix(a, c, t), mix(c, b, t), t);
}

// A lobe's own stem: from the clump's base out through the lobe, leaving
// along the clump's twig and ending past the lobe's centre; its sprays'
// twigs leave it along its length (those facing back toward the base
// low down, those facing out near its end), a branchlet, not a star. Its
// joint `k` (of FOREST_STEM_PIECES), crooked; `lk` the lobe's stream.
uint forestLobeStream(ForestClumpShoots c, int lobe) {
    return procHash(c.sb ^ uint(lobe + 31) * 0x85ebca6bu);
}

vec3 forestLobeStemJoint(ForestClumpShoots c, vec3 lc, float lr, uint lk, int k, out vec3 tangent) {
    vec3 o = lc - c.B;
    o = dot(o, o) > 1e-10 ? normalize(o) : c.D;
    vec3 Z = lc + o * (0.3 * lr);
    float len = length(Z - c.B);
    vec3 ctrl = c.B + c.D * (0.5 * len);
    vec3 p = forestQuadBezier(c.B, ctrl, Z, float(k) / float(FOREST_STEM_PIECES), tangent);
    if (k > 0) p += forestCrook(lk, k, tangent, FOREST_CROOK * len / float(FOREST_STEM_PIECES));
    return p;
}

// The point `t` (0..1) along a lobe's stem as drawn, and its way there.
vec3 forestLobeStemAtT(ForestClumpShoots c, vec3 lc, float lr, uint lk, float t, out vec3 tangent) {
    int k0;
    float f;
    forestPieceAt(t, FOREST_STEM_PIECES, k0, f);
    vec3 ta, tb;
    vec3 a = forestLobeStemJoint(c, lc, lr, lk, k0, ta), b = forestLobeStemJoint(c, lc, lr, lk, k0 + 1, tb);
    return forestHermite(a, ta, b, tb, f, tangent);
}

vec3 forestLobeStemAt(ForestClumpShoots c, vec3 lc, float lr, uint lk, float t) {
    vec3 tangent;
    return forestLobeStemAtT(c, lc, lr, lk, t, tangent);
}

// A lobe's lattice: its way out from the clump's base (`o`), two ways
// across it, and the lattice's turn (its stream).
void forestLobeFrame(ForestClumpShoots c, int lobe, vec3 lc, out vec3 o, out vec3 e1, out vec3 e2, out uint lh) {
    o = lc - c.B;
    o = dot(o, o) > 1e-10 ? normalize(o) : c.D;
    e1 = perpendicularUnit(o);
    e2 = cross(o, e1);
    lh = procHash(c.sb ^ uint(lobe + 7) * 0x85ebca6bu);
}

ForestSpray forestSpray(ForestClumpShoots c, int b) {
    ForestSpray s;
    int lobe = b % 3;
    int i = b / 3;
    int L = forestLobeSprays(c.NB, lobe);
    procClumpLobe(vec4(c.C, c.r), c.clump, lobe, c.seed, s.lc, s.lr);
    s.hb = procHash(c.sb + uint(b + 1) * 0x9e3779b9u);
    s.lobe = lobe;

    // A Fibonacci lattice over the lobe's sphere about its way out from the
    // clump's base, each point jittered within its cell.
    vec3 o, e1, e2;
    uint lh;
    forestLobeFrame(c, lobe, s.lc, o, e1, e2, lh);
    float fl = float(max(L, 1));
    float z = clamp(1.0 - 2.0 * (float(i) + 0.5) / fl + (procDraw(s.hb, 0u) - 0.5) * 1.2 / fl, -1.0, 1.0);
    float phi = float(i) * 2.39996323 + PROC_TWO_PI * procDraw(lh, 0u) + (procDraw(s.hb, 1u) - 0.5) * 0.6;
    float sz = sqrt(max(0.0, 1.0 - z * z));
    s.u = normalize(o * z + (e1 * cos(phi) + e2 * sin(phi)) * sz);
    s.z = z;
    s.hub = vec3(0.0);
    s.hubWay = o;
    float shell = procedural.header[0].w;
    s.rho = s.lr * (1.0 - shell * (0.2 + 0.6 * procDraw(s.hb, 2u)));

    // Heading away from the clump's base across the sphere, turned a little.
    vec3 E = s.lc + forestShootFlat(s.u * s.rho);
    vec3 away = E - c.B;
    vec3 t = away - s.u * dot(away, s.u);
    t = dot(t, t) > 1e-10 ? normalize(t) : perpendicularUnit(s.u);
    s.h = procRotAxis(t, s.u, (procDraw(s.hb, 3u) * 2.0 - 1.0) * 0.6);
    s.sd = cross(s.u, s.h);
    s.Q = c.S / c.NB + (b < c.S % c.NB ? 1 : 0);
    s.lam = min(float(s.Q) * FOREST_SPRAY_LATERAL * c.Lf * (0.85 + 0.3 * procDraw(s.hb, 4u)), 0.9 * s.rho);
    return s;
}

// A lobe's LIMBS: a lobe of many sprays (a large clump's) does not reach
// each from its stem, a twig across the whole lobe; its stem divides into
// limbs (a Fibonacci lattice of their own over the lobe's sphere, one for
// every FOREST_LIMB_SPRAYS sprays), each from the stem out to just under
// the shell its sprays lie in, and each spray's twig leaves the limb
// nearest it where it passes closest: a short twig across to the spray.
// Limb `g` of `G`: its way out (unit) and its place on the lattice (`z`).
vec3 forestLimbDir(ForestClumpShoots c, int lobe, vec3 lc, int g, int G, out float z) {
    vec3 o, e1, e2;
    uint lh;
    forestLobeFrame(c, lobe, lc, o, e1, e2, lh);
    z = 1.0 - 2.0 * (float(g) + 0.5) / float(G);
    float phi = float(g) * 2.39996323 + PROC_TWO_PI * procDraw(lh, 1u);
    float sz = sqrt(max(0.0, 1.0 - z * z));
    return normalize(o * z + (e1 * cos(phi) + e2 * sin(phi)) * sz);
}

// Limb `g` of a lobe's `G`: its joint `k` of FOREST_LIMB_PIECES, from its
// stem (where a spray's twig facing its way would leave it) to within the
// shell, crooked.
vec3 forestLimbJoint(ForestClumpShoots c, int lobe, int g, int G, int k, out vec3 tangent) {
    vec3 lc;
    float lr;
    procClumpLobe(vec4(c.C, c.r), c.clump, lobe, c.seed, lc, lr);
    float z;
    vec3 d = forestLimbDir(c, lobe, lc, g, G, z);
    uint lk = forestLobeStream(c, lobe);
    float t0 = 0.4 + 0.27 * (z + 1.0);
    // It forks off its stem at an acute angle (halfway between the stem's
    // way there and its own) and bends out to its end.
    vec3 st;
    vec3 a = forestLobeStemAtT(c, lc, lr, lk, t0, st);
    vec3 b = lc + forestShootFlat(d * (lr * 0.95 * (1.0 - 0.8 * procedural.header[0].w)));
    float L = length(b - a);
    vec3 w = normalize(st + normalize(b - a + d * 1e-4));
    vec3 p = forestQuadBezier(a, a + w * (0.5 * L), b, float(k) / float(FOREST_LIMB_PIECES), tangent);
    if (k > 0 && k < FOREST_LIMB_PIECES)
        p += forestCrook(procHash(lk + uint(g + 1) * 0x9e3779b9u), k, tangent, FOREST_CROOK * L / float(FOREST_LIMB_PIECES));
    return p;
}

// Where spray `s`'s twig leaves: the end of its lobe's limb nearest it, or
// (a lobe of few sprays) its stem.
vec3 forestSprayHub(ForestClumpShoots c, ForestSpray s, out vec3 way) {
    int G = forestLobeLimbs(forestLobeSprays(c.NB, s.lobe));
    if (G == 0) {
        float ts = 0.4 + 0.27 * (s.z + 1.0);
        return forestLobeStemAtT(c, s.lc, s.lr, forestLobeStream(c, s.lobe), ts, way);
    }
    int best = 0;
    float bd = -2.0;
    for (int g = 0; g < G; ++g) {
        float z;
        float k = dot(forestLimbDir(c, s.lobe, s.lc, g, G, z), s.u);
        if (k > bd) {
            bd = k;
            best = g;
        }
    }
    // Off the limb where it passes nearest the spray's start, a short
    // twig across to it, not along the whole lobe (beyond its first piece).
    vec3 t;
    vec3 a = forestLimbJoint(c, s.lobe, best, G, 0, t);
    vec3 b = forestLimbJoint(c, s.lobe, best, G, FOREST_LIMB_PIECES, t);
    vec3 E = s.lc + forestShootFlat(s.u * s.rho);
    vec3 ab = b - a;
    float u = clamp(dot(E - a, ab) / max(dot(ab, ab), 1e-10), 1.0 / float(FOREST_LIMB_PIECES), 1.0);
    int k0;
    float f;
    forestPieceAt(u, FOREST_LIMB_PIECES, k0, f);
    vec3 t1;
    vec3 p0 = forestLimbJoint(c, s.lobe, best, G, k0, t), p1 = forestLimbJoint(c, s.lobe, best, G, k0 + 1, t1);
    return forestHermite(p0, t, p1, t1, f, way);
}

// The zigzag's step at joint `k` of a twig heading `T` with leaves spreading
// toward `side`: to alternate sides, a little out of the plane too. At most
// FOREST_ZIGZAG_PIECE of its pieces' length (`piece`): a short shoot's
// pieces are shorter than its internodes, and stepped aside further each
// piece's chord turns so far off the twig's way at its joints (each two
// meeting there at twice it) that the curve through them bends at each
// joint as sharply as a corner.
const float FOREST_ZIGZAG_PIECE = 0.12;
vec3 forestZigzag(ForestClumpShoots c, uint h, int k, vec3 T, vec3 side, float piece) {
    float amp = min((c.phyllotaxis == 0 ? FOREST_ZIGZAG.x : FOREST_ZIGZAG.y) * c.Lf, FOREST_ZIGZAG_PIECE * piece);
    float sgn = (k & 1) != 0 ? 1.0 : -1.0;
    return side * (sgn * amp * (0.6 + 0.8 * procDraw(h, uint(2 * k + 21)))) +
           cross(T, side) * (amp * 0.6 * (procDraw(h, uint(2 * k + 22)) - 0.5));
}

// A spray's twig from its hub (s.hub, forestSprayHub) to its axis' start:
// joint `k` of FOREST_HUB_PIECES, the middle ones crooked.
// It forks off its parent at an acute angle (halfway between the parent's
// way there, s.hubWay, and the spray's start) and comes in along the
// spray's heading: a cubic curve, crooked at its joints.
vec3 forestSprayTwigJoint(ForestSpray s, int k, out vec3 tangent) {
    vec3 E = s.lc + forestShootFlat(s.u * s.rho);
    vec3 he = E - s.hub;
    float len = length(he);
    vec3 w = normalize(s.hubWay + he / max(len, 1e-6));
    vec3 p1 = s.hub + w * (0.35 * len), p2 = E - s.h * (0.35 * len);
    float t = float(k) / float(FOREST_HUB_PIECES), r = 1.0 - t;
    vec3 d = 3.0 * (r * r * (p1 - s.hub) + 2.0 * r * t * (p2 - p1) + t * t * (E - p2));
    tangent = dot(d, d) > 1e-12 ? normalize(d) : normalize(he + vec3(1e-6));
    vec3 p = r * r * r * s.hub + 3.0 * r * t * (r * p1 + t * p2) + t * t * t * E;
    if (k > 0 && k < FOREST_HUB_PIECES) p += forestCrook(s.hb, k, tangent, FOREST_CROOK * len / float(FOREST_HUB_PIECES));
    return p;
}

// A spray's axis: joint `k` of FOREST_AXIS_PIECES (y = k / pieces of its
// length along it), zigzag; its tangent (flattened).
vec3 forestSprayAxisJoint(ForestClumpShoots c, ForestSpray s, int k, out vec3 tangent) {
    float y = s.lam * float(k) / float(FOREST_AXIS_PIECES);
    vec3 T, N;
    vec3 q = forestShootArc(s.u, s.h, s.rho, y, T, N);
    if (k > 0) q += forestZigzag(c, s.hb, k, T, cross(T, N), s.lam / float(FOREST_AXIS_PIECES));
    tangent = normalize(forestShootFlat(T));
    return s.lc + forestShootFlat(q);
}

// The point `y` along a spray's axis as drawn.
vec3 forestSprayAxisAt(ForestClumpShoots c, ForestSpray s, float y) {
    int k0;
    float f;
    forestPieceAt(y / max(s.lam, 1e-5), FOREST_AXIS_PIECES, k0, f);
    vec3 ta, tb, t;
    vec3 a = forestSprayAxisJoint(c, s, k0, ta), b = forestSprayAxisJoint(c, s, k0 + 1, tb);
    return forestHermite(a, ta, b, tb, f, t);
}

// Shoot `q` of a spray (0 its terminal shoot): where it leaves the axis
// (y along it), its start (unflattened, about the lobe's centre), outward
// and heading there, its length and leaves, and how far it is moved to
// leave the axis as drawn (`delta`, flattened).
struct ForestShoot {
    float y;
    vec3 u, h;
    float len;
    int m;
    uint hs;
    vec3 delta;
};

ForestShoot forestShoot(ForestClumpShoots c, ForestSpray s, int shoot, int q) {
    ForestShoot o;
    o.m = c.n / c.S + (shoot < c.n % c.S ? 1 : 0);
    o.hs = procHash(s.hb ^ uint(q + 1) * 0xc2b2ae35u);
    o.y = q == 0 ? s.lam : s.lam * float(s.Q - q) / float(max(s.Q, 1));
    vec3 T, N;
    forestShootArc(s.u, s.h, s.rho, o.y, T, N);
    o.u = N;
    if (q == 0) {
        o.h = T;
    } else {
        float side = (q & 1) != 0 ? 1.0 : -1.0;
        o.h = normalize(T * cos(FOREST_SHOOT_ANGLE) + s.sd * (side * sin(FOREST_SHOOT_ANGLE)));
    }
    o.len = min(float(max(o.m, 1)) * FOREST_SHOOT_INTERNODE * c.Lf * (0.85 + 0.3 * procDraw(o.hs, 0u)), s.rho);
    o.delta = forestSprayAxisAt(c, s, o.y) - (s.lc + forestShootFlat(N * s.rho));
    return o;
}

// A shoot's joint `k` of FOREST_SHOOT_PIECES (x = k / pieces of its length),
// zigzag in the plane its leaves spread in; its tangent (flattened).
vec3 forestShootJoint(ForestClumpShoots c, ForestSpray s, ForestShoot sh, int k, out vec3 tangent) {
    float x = sh.len * float(k) / float(FOREST_SHOOT_PIECES);
    vec3 T, N;
    vec3 q = forestShootArc(sh.u, sh.h, s.rho, x, T, N);
    if (k > 0) q += forestZigzag(c, sh.hs, k, T, cross(T, N), sh.len / float(FOREST_SHOOT_PIECES));
    // It leaves the axis halfway between the axis' way there and its own
    // (as a limb and a spray's twig leave theirs), so its curve sweeps out
    // of the axis rather than turning off it at a corner.
    if (k == 0) {
        vec3 Ta, Na;
        forestShootArc(s.u, s.h, s.rho, sh.y, Ta, Na);
        T = normalize(T + Ta);
    }
    tangent = normalize(forestShootFlat(T));
    return s.lc + forestShootFlat(q) + sh.delta;
}

// The point `x` along a shoot as drawn.
vec3 forestShootAt(ForestClumpShoots c, ForestSpray s, ForestShoot sh, float x) {
    int k0;
    float f;
    forestPieceAt(x / max(sh.len, 1e-5), FOREST_SHOOT_PIECES, k0, f);
    vec3 ta, tb, t;
    vec3 a = forestShootJoint(c, s, sh, k0, ta), b = forestShootJoint(c, s, sh, k0 + 1, tb);
    return forestHermite(a, ta, b, tb, f, t);
}

// How far along its shoot (0..1) node `i` of `M` lies: closer together
// toward the tip.
float forestShootNode(int i, int M) {
    return 1.0 - pow(max(1.0 - (float(i) + 0.6) / (float(M) + 0.2), 0.0), 1.25);
}

// The spray's and the shoot's own bending, on top of the wood's pose: a
// point `y` along the spray's axis, or `x` along shoot `sh` (y < 0) of it,
// moves by `offset` and turns by `omega` (specimen units and directions; a
// rotation vector). `drag` the wind's at the clump (VegPose::drag),
// `amount` the leaves' own motion kept (forestFlutter).
struct ForestShootSway { float tb, ts; vec3 wb, ws; };

ForestShootSway forestShootSwayAt(ForestSpray s, ForestShoot sh, float drag, float amount) {
    ForestShootSway w;
    float a = amount * sqrt(max(drag, 0.0));
    float fb = 1.4 + 0.8 * procDraw(s.hb, 5u);
    float fs = 2.2 + 1.0 * procDraw(sh.hs, 1u);
    w.tb = a * 0.08 * sin(PROC_TWO_PI * (sceneWindCycles(fb) + procDraw(s.hb, 6u)));
    w.ts = a * 0.12 * sin(PROC_TWO_PI * (sceneWindCycles(fs) + procDraw(sh.hs, 2u)));
    w.wb = normalize(s.u + s.sd * (procDraw(s.hb, 7u) - 0.5));
    w.ws = normalize(sh.u + cross(sh.u, sh.h) * (procDraw(sh.hs, 3u) - 0.5));
    return w;
}

// The offset (specimen units) of a point `y` along the spray's axis.
vec3 forestSprayBend(ForestShootSway w, ForestSpray s, float y) {
    return w.wb * (w.tb * y * y / max(s.lam, 1e-4));
}

// The offset of a point `x` along a shoot leaving the axis at sh.y, and the
// shoot's turn there.
vec3 forestShootBend(ForestShootSway w, ForestSpray s, ForestShoot sh, float x, out vec3 omega) {
    float L = max(sh.len, 1e-4);
    omega = cross(sh.h, w.ws) * (w.ts * 2.0 * x / L) + cross(s.h, w.wb) * (w.tb * 2.0 * sh.y / max(s.lam, 1e-4));
    return forestSprayBend(w, s, sh.y) + w.ws * (w.ts * x * x / L);
}

// Leaf `j` (its key less the clump's base) of a clump: its node `P`, its
// frame `R` (+X across, +Y the card's normal, +Z its tip; scaled) and tip
// direction `F`, the lobe it lies in (`lc`, `lr`), and where on its spray
// and shoot it hangs (for forestShootBend). False for a leaf with no
// extent.
struct ForestLeafAt {
    ForestSpray spray;
    ForestShoot shoot;
    float x;
};

bool forestShootLeaf(ForestClumpShoots c, int j, out vec3 P, out mat3 R, out vec3 F, out vec3 lc, out float lr,
                     out ForestLeafAt at) {
    int shootIx = j % c.S;
    int k = j / c.S;
    int b = shootIx % c.NB;
    int q = shootIx / c.NB;
    ForestSpray s = forestSpray(c, b);
    ForestShoot sh = forestShoot(c, s, shootIx, q);
    lc = s.lc;
    lr = s.lr;
    at.spray = s;
    at.shoot = sh;

    int m = sh.m;
    bool opposite = c.phyllotaxis == 1;
    int M = opposite ? (m + 1) / 2 : m;
    int i = opposite ? k / 2 : k;
    float tn = forestShootNode(i, M);
    float x = sh.len * tn;
    at.x = x;
    vec3 T, Nn;
    forestShootArc(sh.u, sh.h, s.rho, x, T, Nn);
    // On the twig as drawn: the chord of its piece of the shoot.
    P = forestShootAt(c, s, sh, x);

    uint hl = procHash(sh.hs + uint(k + 1) * 0x27d4eb2fu);
    // Its azimuth about the shoot: two ranks, opposite pairs turning a
    // quarter at each node, or a 2/5 spiral.
    float psi = c.phyllotaxis == 0 ? 3.14159265 * float(i & 1)
              : c.phyllotaxis == 1 ? 1.5707963 * float(i & 1) + 3.14159265 * float(k & 1)
              : float(i) * 2.5132741;
    psi += (procDraw(hl, 0u) - 0.5) * 0.5;
    vec3 Bn = cross(T, Nn);
    vec3 e = Bn * cos(psi) + Nn * sin(psi);
    float delta = FOREST_LEAF_ANGLE + (procDraw(hl, 1u) - 0.5) * 0.35;
    vec3 F0 = T * cos(delta) + e * sin(delta);

    // Turned toward the light: into the spray's plane, facing out of the
    // crown and up; drooping.
    float upBias = procedural.header[0].y;
    float tiltJitter = procedural.header[0].z;
    vec3 Nt = normalize(Nn * (1.0 - upBias) + vec3(0.0, upBias, 0.0));
    F = F0 - Nt * (FOREST_LEAF_FLATTEN * dot(F0, Nt)) - vec3(0.0, FOREST_LEAF_DROOP, 0.0);
    F = dot(F, F) > 1e-10 ? normalize(F) : T;
    vec3 jit = vec3(procDraw(hl, 2u), procDraw(hl, 3u), procDraw(hl, 4u)) * 2.0 - 1.0;
    vec3 N = Nt + jit * (FOREST_LEAF_TILT * tiltJitter);
    N -= F * dot(N, F);
    N = dot(N, N) > 1e-10 ? normalize(N) : perpendicularUnit(F);
    vec3 X = cross(F, N);

    vec4 h1 = procedural.header[1];
    float grade = 0.8 + 0.35 * smoothstep(0.0, 0.5, tn) - 0.12 * smoothstep(0.8, 1.0, tn);
    float scale = h1.x * (1.0 + (procDraw(hl, 5u) * 2.0 - 1.0) * h1.y) * grade;
    R = mat3(1.0);
    if (scale < 1e-6) return false;
    R = mat3(X * scale, N * scale, F * scale);
    return true;
}

// Twig piece `p` of a clump: one end of it (`end` 0 its start, 1 its end;
// specimen units, at rest), the twig's way there, its radius, how far
// along the whole twig it is (0..1, `frac`) and along its spray's axis or
// shoot (`along`; for forestShootBend), and which: 0 a lobe's stem (no
// bend of its own), 1 a spray's twig or axis, 2 a shoot. False for a piece
// that is not there (a lobe with no sprays).
// A stem's, limb's, axis' or shoot's end is this much of the radius it
// would have there: its last piece tapers to a point, not a cut as wide as
// the wood going into it.
const float FOREST_TWIG_TIP = 0.25;

struct ForestTwigEnd {
    vec3 p, t;
    float radius;
    float frac;
    float along;   // y on the axis (kind 1) or x on the shoot (kind 2); 0 on a hub's twig
    bool base;     // the start of a twig leaving the wood it grows from (a stem, limb, spray's twig, shoot)
    float start;   // the piece's radius at its start, whichever end this is (what its cull reads)
};

bool forestTwigEnd(ForestClumpShoots c, int p, float end, out ForestTwigEnd o, out int kind, out ForestSpray s,
                   out ForestShoot sh) {
    o.along = 0.0;
    o.base = false;
    kind = 0;
    sh.y = 0.0;
    sh.u = vec3(0.0, 1.0, 0.0);
    sh.h = vec3(1.0, 0.0, 0.0);
    sh.len = 1.0;
    sh.m = 0;
    sh.hs = 0u;
    sh.delta = vec3(0.0);
    float Lf = c.Lf;
    int e = int(end + 0.5);
    // The shoots a spray carries, on the average.
    float Qm = float(c.S) / float(max(c.NB, 1));
    if (p < FOREST_STEMS) {
        int lobe = p / FOREST_STEM_PIECES;
        if (lobe >= c.NB) return false;
        vec3 lc;
        float lr;
        procClumpLobe(vec4(c.C, c.r), c.clump, lobe, c.seed, lc, lr);
        int k = p - lobe * FOREST_STEM_PIECES + e;
        o.p = forestLobeStemJoint(c, lc, lr, forestLobeStream(c, lobe), k, o.t);
        o.frac = float(k) / float(FOREST_STEM_PIECES);
        o.base = k == 0;
        // All its lobe's sprays at its foot, two sprays' worth at its end.
        float L = float(forestLobeSprays(c.NB, lobe));
        o.radius = Lf * mix(forestPipe(L * Qm), forestPipe(min(L, 2.0) * Qm), o.frac);
        o.start = Lf * mix(forestPipe(L * Qm), forestPipe(min(L, 2.0) * Qm), float(k - e) / float(FOREST_STEM_PIECES));
        if (k == FOREST_STEM_PIECES) o.radius *= FOREST_TWIG_TIP;
        return true;
    }
    int slots = forestLimbSlots(c.NB) * FOREST_LIMB_PIECES;
    int sprays = FOREST_STEMS + 3 * slots;
    if (p < sprays) {
        int lobe = (p - FOREST_STEMS) / slots;
        int q = (p - FOREST_STEMS) - lobe * slots;
        int g = q / FOREST_LIMB_PIECES;
        int G = forestLobeLimbs(forestLobeSprays(c.NB, lobe));
        if (g >= G) return false;
        int k = q - g * FOREST_LIMB_PIECES + e;
        o.p = forestLimbJoint(c, lobe, g, G, k, o.t);
        o.frac = float(k) / float(FOREST_LIMB_PIECES);
        o.base = k == 0;
        float carried = float(forestLobeSprays(c.NB, lobe)) / float(G) * Qm;
        o.radius = Lf * mix(forestPipe(carried), forestPipe(carried * 0.6), o.frac);
        o.start = Lf * mix(forestPipe(carried), forestPipe(carried * 0.6), float(k - e) / float(FOREST_LIMB_PIECES));
        if (k == FOREST_LIMB_PIECES) o.radius *= FOREST_TWIG_TIP;
        return true;
    }
    int pre = sprays + FOREST_SPRAY_PIECES * c.NB;
    if (p < pre) {
        int b = (p - sprays) / FOREST_SPRAY_PIECES;
        int r = (p - sprays) - b * FOREST_SPRAY_PIECES + e;
        s = forestSpray(c, b);
        kind = 1;
        if (r < FOREST_HUB_PIECES || (r == FOREST_HUB_PIECES && e == 1)) {
            s.hub = forestSprayHub(c, s, s.hubWay);
            o.p = forestSprayTwigJoint(s, r, o.t);
            o.frac = float(r) / float(FOREST_HUB_PIECES);
            o.base = r == 0;
            o.radius = Lf * forestPipe(float(s.Q)) * (1.08 - 0.08 * o.frac);
            o.start = Lf * forestPipe(float(s.Q)) * (1.08 - 0.08 * float(r - e) / float(FOREST_HUB_PIECES));
            return true;
        }
        int k = r - FOREST_HUB_PIECES;
        o.p = forestSprayAxisJoint(c, s, k, o.t);
        o.frac = float(k) / float(FOREST_AXIS_PIECES);
        // Its shoots leave it along its length, its terminal one at its end.
        o.radius = Lf * forestPipe(1.0 + float(s.Q - 1) * (1.0 - o.frac));
        o.start = Lf * forestPipe(1.0 + float(s.Q - 1) * (1.0 - float(max(k - e, 0)) / float(FOREST_AXIS_PIECES)));
        if (k == FOREST_AXIS_PIECES) o.radius *= FOREST_TWIG_TIP;
        o.along = s.lam * o.frac;
        return true;
    }
    int shootIx = (p - pre) / FOREST_SHOOT_PIECES;
    if (shootIx >= c.S) return false;
    int k = (p - pre) - shootIx * FOREST_SHOOT_PIECES + e;
    int b = shootIx % c.NB;
    s = forestSpray(c, b);
    sh = forestShoot(c, s, shootIx, shootIx / c.NB);
    kind = 2;
    o.p = forestShootJoint(c, s, sh, k, o.t);
    o.frac = float(k) / float(FOREST_SHOOT_PIECES);
    o.base = k == 0;
    o.radius = Lf * mix(FOREST_SHOOT_RADIUS.x, FOREST_SHOOT_RADIUS.y, o.frac);
    o.start = Lf * mix(FOREST_SHOOT_RADIUS.x, FOREST_SHOOT_RADIUS.y, float(k - e) / float(FOREST_SHOOT_PIECES));
    if (k == FOREST_SHOOT_PIECES) o.radius *= FOREST_TWIG_TIP;
    o.along = sh.len * o.frac;
    return true;
}

// A pinnate clump's leaf (phyllotaxis 3: a palm frond's leaflets): no twigs,
// its leaves line the wood the clump grows on, through its base and a
// clump's radius either way along its twig direction, in two ranks either
// side, each angled forward and drooping (a frond's V), facing up. They
// take no spray or shoot bend (`at` holds a still one): they ride the
// wood's pose.
const float FOREST_PINNATE_FORWARD = 0.45;   // radians a leaflet leans toward the tip
const float FOREST_PINNATE_DROOP = 0.35;     // how far it hangs below its rank's level
bool forestPinnateLeaf(ForestClumpShoots c, int j, out vec3 P, out mat3 R, out vec3 F, out vec3 lc, out float lr,
                       out ForestLeafAt at) {
    lc = c.C;
    lr = c.r;
    at.spray.lc = c.C; at.spray.lr = c.r; at.spray.hub = c.B; at.spray.hubWay = c.D;
    at.spray.u = vec3(0.0, 1.0, 0.0); at.spray.h = vec3(1.0, 0.0, 0.0); at.spray.sd = vec3(0.0, 0.0, 1.0);
    at.spray.rho = 1.0; at.spray.lam = 1.0; at.spray.Q = 1; at.spray.hb = 0u; at.spray.lobe = 0; at.spray.z = 0.0;
    at.shoot.y = 0.0; at.shoot.u = vec3(0.0, 1.0, 0.0); at.shoot.h = vec3(1.0, 0.0, 0.0);
    at.shoot.len = 1.0; at.shoot.m = 0; at.shoot.hs = 0u; at.shoot.delta = vec3(0.0);
    at.x = 0.0;
    uint hl = procHash(c.sb + uint(j + 1) * 0x27d4eb2fu);
    int pairs = max((c.n + 1) / 2, 1);
    float t = (float(j / 2) + 0.5 + (procDraw(hl, 0u) - 0.5) * 0.4) / float(pairs);
    P = c.B + c.D * (c.r * (2.0 * t - 1.0));
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 S = cross(c.D, up);
    S = dot(S, S) > 1e-6 ? normalize(S) : perpendicularUnit(c.D);
    vec3 U = cross(S, c.D);
    // Wood that hangs (a willow's whips) holds its leaves all round it, not
    // in the plane of two ranks a level frond spreads: each rank turns about
    // the wood by up to a quarter turn either way as it hangs.
    float hang = smoothstep(0.3, 0.8, -c.D.y);
    float turn = (procDraw(hl, 3u) - 0.5) * 3.14159265 * hang;
    vec3 S0 = S;
    S = S0 * cos(turn) + U * sin(turn);
    U = U * cos(turn) - S0 * sin(turn);
    float side = (j & 1) == 0 ? 1.0 : -1.0;
    float fwd = FOREST_PINNATE_FORWARD + (procDraw(hl, 1u) - 0.5) * 0.2;
    F = normalize(S * (side * cos(fwd)) + c.D * sin(fwd) - U * (FOREST_PINNATE_DROOP + (procDraw(hl, 2u) - 0.5) * 0.2));
    vec3 N = U - F * dot(U, F);
    N = dot(N, N) > 1e-10 ? normalize(N) : perpendicularUnit(F);
    vec3 X = cross(F, N);
    vec4 h1 = procedural.header[1];
    float scale = h1.x * (1.0 + (procDraw(hl, 5u) * 2.0 - 1.0) * h1.y);
    R = mat3(1.0);
    if (scale < 1e-6) return false;
    R = mat3(X * scale, N * scale, F * scale);
    return true;
}

// Leaf `lf` (forestLeaf's clump and key) of instance `i`, as every forest
// leaf stage places it: a broad node's on its shoot (`onShoot`, `at` where),
// a spray node's as the clump-mode scatter places it.
bool forestLeafFrame(int i, ivec2 lf, out vec3 P, out mat3 R, out vec3 F, out vec3 lc, out float lr, out bool onShoot,
                     out ForestLeafAt at) {
    onShoot = forestLeafForm() == 0;
    if (!onShoot) return clumpLeafFrame(procedural.records[forestClumpRecord(lf.x)], lf.y, lf.x, P, R, F, lc, lr);
    int phyllotaxis = int(forestSpecimenShoot(forestInstanceSpecimen(i)).y + 0.5);
    ForestClumpShoots c = forestClumpShoots(lf.x, phyllotaxis);
    int j = lf.y - forestClumpKeyBase(lf.x);
    if (j < 0 || j >= c.n) return false;
    if (phyllotaxis == 3) return forestPinnateLeaf(c, j, P, R, F, lc, lr, at);
    return forestShootLeaf(c, j, P, R, F, lc, lr, at);
}

// A shoot leaf's own share of its spray's and shoot's bending, in world
// terms for a tree at `model`: the offset its node moves by and the turn
// (a rotation vector) it takes.
void forestLeafShootSway(ForestLeafAt at, mat4 model, float drag, float amount, out vec3 offset, out vec3 omega) {
    ForestShootSway w = forestShootSwayAt(at.spray, at.shoot, drag, amount);
    vec3 om;
    offset = mat3(model) * forestShootBend(w, at.spray, at.shoot, at.x, om);
    omega = mat3(model) * om / vegScale(model);
}
