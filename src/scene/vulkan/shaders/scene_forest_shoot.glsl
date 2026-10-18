// A broad forest's leaves on their twigs (scene_forest.glsl's leaf form 0):
// every leaf hangs from a node of a current-year SHOOT, the shoots branch
// off a SPRAY's axis, each spray's axis is reached by a twig from its
// LOBE's hub, and the hubs by stems from the clump's base, the end of the
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
// The twigs (mesh_forest_tube.vert's twig build) are the same curves drawn
// as camera-facing ribbons, a PIECE at a time: 6 lobe stems (two a lobe),
// 5 a spray (two from its hub, three along its axis), 3 a shoot. A twig's
// point and a leaf's node go through the same functions, so the two meet
// exactly, and move together: both take the wood's pose (vegPose) of the
// clump's record where they are, then the spray's and the shoot's own
// bending (forestShootSway), which only adds to the motion the leaf's
// flutter about its petiole rides on.
//
// Needs scene_procedural.glsl, scene_vegetation.glsl (sceneWindCycles) and
// scene_forest.glsl.

const int FOREST_SHOOT_LEAVES = 8;
const int FOREST_SPRAY_SHOOTS = 5;
const float FOREST_SHOOT_INTERNODE = 0.6;    // leaf lengths between a shoot's nodes
const float FOREST_SPRAY_LATERAL = 1.1;      // leaf lengths along a spray's axis between its shoots
const float FOREST_SHOOT_ANGLE = 1.0;        // radians a lateral shoot leaves its axis at
const float FOREST_LEAF_ANGLE = 0.95;        // radians a blade leaves its shoot at
// How far a blade turns into its spray's plane (1 flat in it), how far it
// droops, and how much of the scatter's tilt jitter its normal keeps.
const float FOREST_LEAF_FLATTEN = 0.6;
const float FOREST_LEAF_DROOP = 0.12;
const float FOREST_LEAF_TILT = 1.0;

// The shoots and sprays a clump of `n` leaves carries.
void forestShootCounts(int n, out int S, out int NB) {
    S = max(1, (n + 4) / FOREST_SHOOT_LEAVES);
    NB = S < 3 ? S : max(3, (S + 2) / FOREST_SPRAY_SHOOTS);
}

// A clump's twig pieces: its lobe stems and sprays (`withShoots` false), or
// all of them. ForestNode::twigPieces counts the same.
int forestTwigPieces(int n, bool withShoots) {
    if (n <= 0) return 0;
    int S, NB;
    forestShootCounts(n, S, NB);
    return 6 + 5 * NB + (withShoots ? 3 * S : 0);
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

// Spray `b`: its lobe, its hub (where its twig leaves the lobe's stem), its
// axis' start on the lobe's shell (E, outward u, heading h, across sd), its
// length and the sphere its shoots follow (radius rho about the lobe's
// centre), its shoots.
struct ForestSpray {
    vec3 lc;
    float lr;
    vec3 hub;
    vec3 u, h, sd;
    float rho, lam;
    int Q;          // its shoots
    uint hb;
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

// A lobe's own twig: from the clump's base out through the lobe, leaving
// along the clump's twig and ending past the lobe's centre; its sprays'
// twigs leave it along its length (those facing back toward the base
// low down, those facing out near its end), a branchlet, not a star.
vec3 forestLobeAxis(ForestClumpShoots c, vec3 lc, float lr, float t, out vec3 tangent) {
    vec3 o = lc - c.B;
    o = dot(o, o) > 1e-10 ? normalize(o) : c.D;
    vec3 Z = lc + o * (0.45 * lr);
    vec3 ctrl = c.B + c.D * (0.5 * length(Z - c.B));
    return forestQuadBezier(c.B, ctrl, Z, t, tangent);
}

ForestSpray forestSpray(ForestClumpShoots c, int b) {
    ForestSpray s;
    int lobe = b % 3;
    int i = b / 3;
    int L = c.NB / 3 + (lobe < c.NB % 3 ? 1 : 0);
    procClumpLobe(vec4(c.C, c.r), c.clump, lobe, c.seed, s.lc, s.lr);
    s.hb = procHash(c.sb + uint(b + 1) * 0x9e3779b9u);
    uint lh = procHash(c.sb ^ uint(lobe + 7) * 0x85ebca6bu);

    // A Fibonacci lattice over the lobe's sphere about its way out from the
    // clump's base, each point jittered within its cell.
    vec3 o = s.lc - c.B;
    o = dot(o, o) > 1e-10 ? normalize(o) : c.D;
    vec3 e1 = perpendicularUnit(o);
    vec3 e2 = cross(o, e1);
    float fl = float(max(L, 1));
    float z = clamp(1.0 - 2.0 * (float(i) + 0.5) / fl + (procDraw(s.hb, 0u) - 0.5) * 1.2 / fl, -1.0, 1.0);
    float phi = float(i) * 2.39996323 + PROC_TWO_PI * procDraw(lh, 0u) + (procDraw(s.hb, 1u) - 0.5) * 0.6;
    float sz = sqrt(max(0.0, 1.0 - z * z));
    s.u = normalize(o * z + (e1 * cos(phi) + e2 * sin(phi)) * sz);
    vec3 at;
    s.hub = forestLobeAxis(c, s.lc, s.lr, 0.4 + 0.27 * (z + 1.0), at);
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

// Shoot `q` of a spray (0 its terminal shoot): where it leaves the axis
// (y along it), its start (unflattened, about the lobe's centre), outward
// and heading there, its length and leaves.
struct ForestShoot {
    float y;
    vec3 u, h;
    float len;
    int m;
    uint hs;
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
    return o;
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
    vec3 p = forestShootArc(sh.u, sh.h, s.rho, x, T, Nn);
    // On the twig as drawn: the chord of its third of the shoot (the twig
    // is three straight pieces), not the arc a few millimetres off it.
    float u3 = 3.0 * x / max(sh.len, 1e-5);
    float r3 = min(floor(u3), 2.0);
    vec3 Ta, Na;
    vec3 pa = forestShootArc(sh.u, sh.h, s.rho, sh.len * r3 / 3.0, Ta, Na);
    vec3 pb = forestShootArc(sh.u, sh.h, s.rho, sh.len * (r3 + 1.0) / 3.0, Ta, Na);
    P = s.lc + forestShootFlat(mix(pa, pb, u3 - r3));

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

// Twig piece `p` of a clump: its two ends (`a`, `b`, specimen units, at rest)
// and tangents there, their radii, how far each is along its spray's axis or
// shoot (`ya`, `yb`; for forestShootBend), and which: 0 a lobe stem (no bend
// of its own), 1 a spray's twig or axis, 2 a shoot. False for a piece that
// is not there (a lobe with no sprays).
struct ForestTwigEnd {
    vec3 p, t;
    float radius;
    float along;   // y on the axis (kind 1) or x on the shoot (kind 2); 0 on a hub's twig
};

bool forestTwigEnd(ForestClumpShoots c, int p, float end, out ForestTwigEnd o, out int kind, out ForestSpray s,
                   out ForestShoot sh) {
    o.along = 0.0;
    kind = 0;
    sh.y = 0.0;
    sh.u = vec3(0.0, 1.0, 0.0);
    sh.h = vec3(1.0, 0.0, 0.0);
    sh.len = 1.0;
    sh.m = 0;
    sh.hs = 0u;
    float Lf = c.Lf;
    if (p < 6) {
        int lobe = p / 2;
        if (lobe >= c.NB) return false;
        vec3 lc;
        float lr;
        procClumpLobe(vec4(c.C, c.r), c.clump, lobe, c.seed, lc, lr);
        float t = (float(p & 1) + end) * 0.5;
        o.p = forestLobeAxis(c, lc, lr, t, o.t);
        o.radius = Lf * mix(0.045, 0.03, t);
        return true;
    }
    int pre = 6 + 5 * c.NB;
    if (p < pre) {
        int b = (p - 6) / 5;
        int r = (p - 6) - b * 5;
        s = forestSpray(c, b);
        kind = 1;
        if (r < 2) {
            vec3 E = s.lc + forestShootFlat(s.u * s.rho);
            vec3 ctrl = E - s.h * (0.45 * length(E - s.hub));
            float t = (float(r) + end) * 0.5;
            o.p = forestQuadBezier(s.hub, ctrl, E, t, o.t);
            o.radius = Lf * mix(0.028, 0.022, t);
            return true;
        }
        float y = s.lam * (float(r - 2) + end) / 3.0;
        vec3 T, N;
        vec3 q = forestShootArc(s.u, s.h, s.rho, y, T, N);
        o.p = s.lc + forestShootFlat(q);
        o.t = normalize(forestShootFlat(T));
        o.radius = Lf * mix(0.022, 0.016, y / max(s.lam, 1e-4));
        o.along = y;
        return true;
    }
    int shootIx = (p - pre) / 3;
    if (shootIx >= c.S) return false;
    int r = (p - pre) - shootIx * 3;
    int b = shootIx % c.NB;
    s = forestSpray(c, b);
    sh = forestShoot(c, s, shootIx, shootIx / c.NB);
    kind = 2;
    float x = sh.len * (float(r) + end) / 3.0;
    vec3 T, N;
    vec3 q = forestShootArc(sh.u, sh.h, s.rho, x, T, N);
    o.p = s.lc + forestShootFlat(q);
    o.t = normalize(forestShootFlat(T));
    o.radius = Lf * mix(0.014, 0.008, x / max(sh.len, 1e-4));
    o.along = x;
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
