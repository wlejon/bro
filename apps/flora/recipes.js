// Procedural plant recipes — composed from bromesh primitives in JS.
//
// Recipes return:
//   { parts: [{ mesh, color, metallic?, roughness? }], aabbMin, aabbMax }
// where parts is one or more sub-meshes, each with its own material. The
// flora app instantiates one scene node per part.
//
// Goal: stylized, instantly-recognizable silhouettes. Tree = trunk +
// noise-displaced green blob (lollipop). Conifer = trunk + stacked green
// cones (Christmas tree). Shrub = cluster of small blobs. Skeleton work
// (space colonization, sweep, meshBranches) still used where it earns
// silhouette character (broadleaf branching shows below the canopy).

(function (root) {

// ─── Math helpers ─────────────────────────────────────────────────────────

const TAU = Math.PI * 2;

function v3(x, y, z) { return [x, y, z]; }
function vAdd(a, b) { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function vSub(a, b) { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
function vScale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
function vDot(a, b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
function vCross(a, b) {
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
}
function vLen(a) { return Math.sqrt(vDot(a, a)); }
function vNorm(a) {
    const L = vLen(a);
    return L > 1e-8 ? [a[0]/L, a[1]/L, a[2]/L] : [0, 1, 0];
}
function vNormOr(a, fallback) {
    const L = vLen(a);
    return L > 1e-8 ? [a[0]/L, a[1]/L, a[2]/L] : fallback;
}

function mulberry32(seed) {
    let s = (seed >>> 0) || 1;
    return function () {
        s = (s + 0x6D2B79F5) >>> 0;
        let t = s;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function smoothstep(edge0, edge1, x) {
    const t = Math.max(0, Math.min(1, (x - edge0) / Math.max(1e-6, edge1 - edge0)));
    return t * t * (3 - 2 * t);
}

// ─── Canopy palette ───────────────────────────────────────────────────────
//
// Stylized greens. Slight per-recipe variation so a forest of plants has
// some chromatic mix without going full saturated.
const COLORS = {
    bark:        [0.42, 0.28, 0.16],
    barkLight:   [0.50, 0.36, 0.22],
    barkDark:    [0.30, 0.20, 0.12],

    canopyOak:   [0.30, 0.55, 0.22],
    canopyMaple: [0.42, 0.62, 0.20],
    canopyPine:  [0.18, 0.40, 0.20],
    canopyShrub: [0.32, 0.58, 0.24],
    canopyVine:  [0.34, 0.56, 0.22],

    succulent:   [0.40, 0.62, 0.32],
    fernLeaf:    [0.30, 0.50, 0.22],
    grassBlade:  [0.36, 0.58, 0.22],
};

// ─── Common builders ──────────────────────────────────────────────────────

function emptyAabb() { return { min: [Infinity,Infinity,Infinity], max: [-Infinity,-Infinity,-Infinity] }; }

function aabbInclude(aabb, p, r) {
    r = r || 0;
    if (p[0] - r < aabb.min[0]) aabb.min[0] = p[0] - r;
    if (p[1] - r < aabb.min[1]) aabb.min[1] = p[1] - r;
    if (p[2] - r < aabb.min[2]) aabb.min[2] = p[2] - r;
    if (p[0] + r > aabb.max[0]) aabb.max[0] = p[0] + r;
    if (p[1] + r > aabb.max[1]) aabb.max[1] = p[1] + r;
    if (p[2] + r > aabb.max[2]) aabb.max[2] = p[2] + r;
}

function circleProfile(n, r) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const a = TAU * i / n;
        out.push([Math.cos(a) * r, Math.sin(a) * r]);
    }
    return out;
}

// Build an apex-up cone by sweeping a unit circle profile from base radius
// down to zero along a vertical path. Mesh.cone's bromesh wrapper has a
// non-canonical Y range; this primitive is deterministic.
function buildCone(baseRadius, height, slices, stacks) {
    slices = slices || 12;
    stacks = stacks || 2;
    const profile = circleProfile(slices, 1);
    const path = [];
    const profileScale = [];
    for (let i = 0; i <= stacks; i++) {
        const t = i / stacks;
        path.push([0, t * height, 0]);
        profileScale.push(baseRadius * (1 - t));
    }
    return Mesh.sweep(profile, path, {
        closeProfile: true,
        capStart: true,
        capEnd: false,
        miterJoints: false,
        profileScale,
    });
}

// Build a noise-displaced sphere translated to `center` and scaled. Returns
// a Mesh handle. `nsub` = subdivision level (2 → ~80 tris, 3 → ~320).
function buildBlob(center, radius, seed, opts) {
    opts = opts || {};
    const nsub = opts.nsub ?? 2;
    const sx = opts.sx ?? 1;
    const sy = opts.sy ?? 1;
    const sz = opts.sz ?? 1;
    const m = Mesh.rock(radius, seed | 0, nsub);
    if (sx !== 1 || sy !== 1 || sz !== 1) m.scale(sx, sy, sz);
    m.translate(center[0], center[1], center[2]);
    return m;
}

// ─── Canopy shape catalogue ───────────────────────────────────────────────
//
// Tree canopies are composed of one or more noise-displaced "blobs" arranged
// to fit a named silhouette. Each shape is a function of (R, H, seed, ...)
// and produces an array of blob descriptors (local-space offset, scale,
// noise sub level, blob radius). The tree() recipe materialises them.
//
// Shapes:
//   round       - single oblate sphere (oak-like default)
//   oval        - vertically elongated ellipsoid (poplar/birch)
//   columnar    - tall, narrow, multi-stack (Lombardy poplar)
//   umbrella    - wide flat top, thin disc-like (acacia, parasol pine)
//   weeping     - round head + drooping streamer blobs (willow)
//   vase        - inverted cone, wider at the top (elm)
//   spreading   - flat wide canopy with side lobes (live oak)
//   irregular   - asymmetric multi-blob cluster (windswept / mature oak)

const CANOPY_SHAPES = ['round', 'oval', 'columnar', 'umbrella', 'weeping', 'vase', 'spreading', 'irregular'];

// Build canopy blob parts for a given shape.
//   ctx.center      - canopy centroid in world space
//   ctx.radius      - nominal horizontal canopy radius (R)
//   ctx.height      - canopy vertical extent (H)
//   ctx.color       - canopy color
//   ctx.seed        - rng seed
//   ctx.shift       - [dx,dy,dz] world-space shift (canopy lean for sharing)
//   ctx.asymmetry   - 0..1 strength of side-of-shift squash (forest sharing)
//   ctx.blobCount   - hint for irregular shape
function buildTreeCanopy(shape, ctx) {
    const center = ctx.center;
    const R = ctx.radius;
    const H = ctx.height;
    const color = ctx.color;
    const seed = (ctx.seed | 0) || 1;
    const shift = ctx.shift || [0, 0, 0];
    const asym = Math.max(0, Math.min(1, ctx.asymmetry ?? 0));
    const blobCount = Math.max(1, ctx.blobCount ?? 1);
    const rng = mulberry32(seed);

    const sLen = Math.sqrt(shift[0]*shift[0] + shift[2]*shift[2]);
    const sdir = sLen > 1e-6 ? [shift[0]/sLen, 0, shift[2]/sLen] : null;

    const parts = [];
    const aabb = emptyAabb();

    // Apply per-blob asymmetric squash: blobs on the side OPPOSITE to shift
    // (that is, leaning INTO a neighbor's canopy) shrink; blobs on the same
    // side as shift (away from neighbor, into open light) grow slightly.
    function asymScale(localOff) {
        if (!sdir || asym === 0) return 1;
        const offLen = Math.sqrt(localOff[0]*localOff[0] + localOff[2]*localOff[2]);
        if (offLen < 1e-6) return 1;
        const d = (localOff[0]*sdir[0] + localOff[2]*sdir[2]) / Math.max(R, 1e-6);
        return Math.max(0.25, 1 + asym * d * 0.7);
    }

    function pushBlob(localOff, r, sx, sy, sz, blobSeed, nsub) {
        const k = asymScale(localOff);
        const sxF = sx * k, szF = sz * k;
        const c = [
            center[0] + localOff[0] + shift[0],
            center[1] + localOff[1] + shift[1],
            center[2] + localOff[2] + shift[2],
        ];
        const m = buildBlob(c, r, blobSeed, { nsub, sx: sxF, sy: sy, sz: szF });
        parts.push({ mesh: m, color, metallic: 0, roughness: 0.85 });
        const ms = Math.max(sxF, sy, szF);
        aabbInclude(aabb, c, r * ms * 1.15);
    }

    switch (shape) {
        case 'oval':
            pushBlob([0, R * 0.18, 0], R * 0.95, 0.85, 1.45, 0.85, seed ^ 0x1002, 3);
            pushBlob([0, -R * 0.25, 0], R * 0.55, 0.7, 1.0, 0.7, seed ^ 0x2002, 2);
            break;
        case 'columnar': {
            const stacks = 4;
            const totalH = Math.max(R * 1.5, H * 0.75);
            for (let i = 0; i < stacks; i++) {
                const t = i / (stacks - 1);
                const widen = 0.55 + 0.15 * Math.sin(Math.PI * t);
                pushBlob(
                    [(rng() - 0.5) * R * 0.05, (t - 0.5) * totalH, (rng() - 0.5) * R * 0.05],
                    R * 0.6,
                    widen, 1.0, widen,
                    (seed * 13 + i * 41) ^ 0x1003,
                    2
                );
            }
            break;
        }
        case 'umbrella':
            pushBlob([0, R * 0.10, 0], R * 1.30, 1.1, 0.40, 1.1, seed ^ 0x1004, 3);
            for (let i = 0; i < 5; i++) {
                const a = TAU * i / 5 + rng() * 0.35;
                pushBlob(
                    [Math.cos(a) * R * 0.95, -R * 0.06, Math.sin(a) * R * 0.95],
                    R * 0.42, 1.0, 0.42, 1.0,
                    (seed * 7 + i * 23) ^ 0x1104, 2
                );
            }
            break;
        case 'weeping':
            pushBlob([0, R * 0.10, 0], R * 1.00, 1.05, 0.90, 1.05, seed ^ 0x1005, 3);
            for (let i = 0; i < 7; i++) {
                const a = TAU * i / 7 + rng() * 0.5;
                const off = R * (0.65 + rng() * 0.25);
                const drop = R * (0.55 + rng() * 0.65);
                pushBlob(
                    [Math.cos(a) * off, -drop, Math.sin(a) * off],
                    R * 0.20 * (0.8 + rng() * 0.4),
                    0.55, 1.55, 0.55,
                    (seed * 19 + i * 17) ^ 0x1105, 1
                );
            }
            break;
        case 'vase': {
            const tiers = 3;
            for (let i = 0; i < tiers; i++) {
                const t = i / (tiers - 1);
                const ringR = R * (0.30 + 0.85 * t);
                const y = (t - 0.45) * H * 0.6;
                const n = i === tiers - 1 ? 5 : 3;
                for (let k = 0; k < n; k++) {
                    const a = TAU * k / n + i * 0.6 + rng() * 0.3;
                    pushBlob(
                        [Math.cos(a) * ringR * 0.65, y, Math.sin(a) * ringR * 0.65],
                        ringR * 0.45 * (0.85 + rng() * 0.30),
                        1.0, 0.95, 1.0,
                        (seed * 23 + i * 31 + k * 11) ^ 0x1006, 2
                    );
                }
            }
            break;
        }
        case 'spreading':
            pushBlob([0, 0, 0], R * 1.30, 1.0, 0.55, 1.0, seed ^ 0x1007, 3);
            for (let i = 0; i < 4; i++) {
                const a = TAU * i / 4 + rng() * 0.4;
                const off = R * 0.80;
                pushBlob(
                    [Math.cos(a) * off, -R * 0.08, Math.sin(a) * off],
                    R * 0.55, 1.0, 0.55, 1.0,
                    (seed * 11 + i * 29) ^ 0x1107, 2
                );
            }
            break;
        case 'irregular': {
            const n = Math.max(3, blobCount);
            for (let i = 0; i < n; i++) {
                const a = TAU * i / n + rng() * 0.45;
                const off = R * 0.55 * (0.65 + rng() * 0.55);
                const yj = (rng() - 0.5) * H * 0.35;
                pushBlob(
                    [Math.cos(a) * off, yj, Math.sin(a) * off],
                    R * 0.50 * (0.75 + rng() * 0.55),
                    1.0, 1.0, 1.0,
                    (seed * 17 + i * 31) ^ 0x1008, 2
                );
            }
            pushBlob([0, 0, 0], R * 0.85, 1.0, 0.85, 1.0, seed ^ 0x2008, 2);
            break;
        }
        case 'round':
        default:
            pushBlob([0, 0, 0], R * 1.15, 1.0, 0.92, 1.0, seed ^ 0x1001, 3);
            break;
    }

    return { parts, aabb };
}

// ─── Recipe: tree (broadleaf) ─────────────────────────────────────────────
// Stylized: trunk + branches + canopy of one of several named shapes.
// The skeleton from space colonization is trimmed to its thicker core so
// branches don't poke through the foliage envelope.

function tree(opts) {
    opts = opts || {};
    const seed = (opts.seed | 0) || 1;
    const age01 = Math.max(0, Math.min(1, opts.age01 ?? 1));
    const H = opts.height ?? 6;
    const trunkRadius = opts.trunkRadius ?? 0.18;
    const CR = opts.canopyRadius ?? 3;
    const blobCount = Math.max(1, opts.blobCount ?? 3);
    const canopyColor = opts.canopyColor || COLORS.canopyOak;
    const canopyShape = opts.canopyShape || 'round';
    const canopyShift = opts.canopyShift || [0, 0, 0];
    const canopyAsymmetry = opts.canopyAsymmetry ?? 0;
    // Per-shape biases on the canopy/trunk envelope.
    const shapeBias = {
        round:     { yPlace: 0.55, vSpan: 0.50, branchUp: 0.30, scaffold: 0.30 },
        oval:      { yPlace: 0.62, vSpan: 0.65, branchUp: 0.45, scaffold: 0.32 },
        columnar:  { yPlace: 0.65, vSpan: 0.85, branchUp: 0.65, scaffold: 0.36 },
        umbrella:  { yPlace: 0.62, vSpan: 0.30, branchUp: 0.20, scaffold: 0.28 },
        weeping:   { yPlace: 0.58, vSpan: 0.45, branchUp: 0.25, scaffold: 0.30 },
        vase:      { yPlace: 0.60, vSpan: 0.55, branchUp: 0.18, scaffold: 0.30 },
        spreading: { yPlace: 0.50, vSpan: 0.30, branchUp: 0.10, scaffold: 0.28 },
        irregular: { yPlace: 0.55, vSpan: 0.55, branchUp: 0.30, scaffold: 0.30 },
    };
    const bias = shapeBias[canopyShape] || shapeBias.round;

    // Effective height/radius scale with age so a young tree is small.
    const Heff = H * Math.max(0.1, age01);
    const CReff = CR * Math.max(0.15, age01 * 0.7 + 0.3);

    const rng = mulberry32(seed);
    const parts = [];
    const aabb = emptyAabb();

    // ── Trunk + main branches via space colonization ────────────────────
    // Attractor cloud is intentionally smaller than the canopy envelope so
    // branches stay tucked inside. The canopy hides everything thinner
    // than the "scaffold" radius. Span varies by canopy shape so columnar
    // trees get an elongated scaffold while spreading trees stay shallow.
    const canopyMidY = Heff * bias.yPlace;
    const canopyBase = Math.max(Heff * 0.18, canopyMidY - Heff * bias.vSpan * 0.5);
    const canopyTop  = canopyMidY + Heff * bias.vSpan * 0.5;
    const attractorCount = 120;
    const attractors = [];
    while (attractors.length < attractorCount) {
        const px = rng() * 2 - 1, py = rng() * 2 - 1, pz = rng() * 2 - 1;
        if (px*px + py*py + pz*pz > 1) continue;
        attractors.push([
            px * CReff * 0.55,
            canopyBase + rng() * (canopyTop - canopyBase),
            pz * CReff * 0.55,
        ]);
    }

    const segLen = Math.max(0.1, Heff * 0.06);
    const trunkTopY = canopyBase * 0.95;
    const trunkSegCount = Math.max(1, Math.round(trunkTopY / segLen));
    const segs = [];
    segs.push({ parent: -1, from: [0,0,0], to: [0,0,0], radius: trunkRadius, depth: 0 });
    let prev = [0, 0, 0];
    for (let i = 1; i <= trunkSegCount; i++) {
        const to = [0, i * segLen, 0];
        segs.push({ parent: segs.length - 1, from: prev, to, radius: 0, depth: i });
        prev = to;
    }
    const trunkTopIdx = segs.length - 1;

    const grown = Mesh.spaceColonize(attractors, [prev], [0, 1, 0], {
        attractionRadius: CReff * 0.7,
        killRadius: Math.max(trunkRadius * 1.5, CReff * 0.10),
        segmentLength: segLen,
        maxIterations: 80,
        tropism: [0, 1, 0],
        tropismWeight: bias.branchUp,
    });
    const offset = segs.length - 1;
    for (let i = 1; i < grown.length; i++) {
        const s = grown[i];
        let p = s.parent;
        if (p === 0) p = trunkTopIdx;
        else if (p > 0) p = p + offset;
        segs.push({ parent: p, from: s.from, to: s.to, radius: 0, depth: s.depth + offset });
    }

    let thick = Mesh.thickenBranches(segs, Math.max(0.01, trunkRadius * 0.18), 2.4);
    let maxR = 0;
    for (const s of thick) if (s.radius > maxR) maxR = s.radius;
    if (maxR > trunkRadius && maxR > 1e-6) {
        const k = trunkRadius / maxR;
        for (const s of thick) s.radius *= k;
    }

    // Trim segments thinner than a "scaffold" threshold — these would be
    // the fuzzy twigs that poke through the foliage and ruin the silhouette.
    const scaffoldR = Math.max(0.01, trunkRadius * bias.scaffold);
    const keepIdx = new Array(thick.length).fill(false);
    for (let i = 0; i < thick.length; i++) {
        if (thick[i].radius >= scaffoldR) keepIdx[i] = true;
    }
    // A segment must have a kept ancestor chain, so reproject parents.
    const remap = new Array(thick.length).fill(-1);
    const kept = [];
    for (let i = 0; i < thick.length; i++) {
        if (!keepIdx[i]) continue;
        const s = thick[i];
        const p = s.parent >= 0 ? remap[s.parent] : -1;
        if (s.parent >= 0 && p === -1) continue; // ancestor was trimmed
        kept.push({ parent: p, from: s.from, to: s.to, radius: s.radius, depth: s.depth });
        remap[i] = kept.length - 1;
    }

    if (kept.length > 0) {
        const trunkMesh = Mesh.meshBranches(kept, 8);
        if (trunkMesh) parts.push({
            mesh: trunkMesh,
            color: COLORS.bark,
            metallic: 0,
            roughness: 0.95,
        });
    }
    for (const s of kept) { aabbInclude(aabb, s.from); aabbInclude(aabb, s.to); }

    // ── Canopy ───────────────────────────────────────────────────────────
    const canopyCenter = [0, (canopyBase + canopyTop) * 0.5, 0];
    const canopyH = Math.max(CReff * 0.5, canopyTop - canopyBase);
    const canopy = buildTreeCanopy(canopyShape, {
        center: canopyCenter,
        radius: CReff,
        height: canopyH,
        color: canopyColor,
        seed,
        shift: canopyShift,
        asymmetry: canopyAsymmetry,
        blobCount,
    });
    for (const p of canopy.parts) parts.push(p);
    aabb.min[0] = Math.min(aabb.min[0], canopy.aabb.min[0]);
    aabb.min[1] = Math.min(aabb.min[1], canopy.aabb.min[1]);
    aabb.min[2] = Math.min(aabb.min[2], canopy.aabb.min[2]);
    aabb.max[0] = Math.max(aabb.max[0], canopy.aabb.max[0]);
    aabb.max[1] = Math.max(aabb.max[1], canopy.aabb.max[1]);
    aabb.max[2] = Math.max(aabb.max[2], canopy.aabb.max[2]);

    if (!isFinite(aabb.min[0])) { aabb.min = [0,0,0]; aabb.max = [0, Heff, 0]; }
    return { parts, aabbMin: aabb.min, aabbMax: aabb.max };
}

// ─── Recipe: conifer ──────────────────────────────────────────────────────
// Stylized Christmas tree: thin trunk + a stack of green cones. Cones grow
// in from the bottom up as age01 advances; each new cone scales from 0 to
// full size over its slice of the age window so growth reads as gradual
// stacking rather than units popping in.

function conifer(opts) {
    opts = opts || {};
    const seed = (opts.seed | 0) || 1;
    const age01 = Math.max(0, Math.min(1, opts.age01 ?? 1));
    const H = opts.height ?? 8;
    const trunkRadius = opts.trunkRadius ?? 0.15;
    const layers = Math.max(3, opts.layers ?? 7);
    const baseCanopyRadius = opts.baseCanopyRadius ?? 2.5;
    const canopyColor = opts.canopyColor || COLORS.canopyPine;

    const Heff = H * Math.max(0.05, age01);
    const parts = [];
    const aabb = emptyAabb();

    // Place cones first so we know how tall the foliage actually reaches —
    // the trunk is then sized to match, avoiding a bare-spike apex during
    // mid-growth ages.
    const baseY = Heff * 0.18;
    const topY = Heff * 1.02;
    const span = topY - baseY;
    const layerH = (span / layers) * 1.45;  // overlap factor
    const tipR = baseCanopyRadius * 0.10 * Math.max(0.2, age01);
    let canopyTopY = baseY;

    const coneParts = [];
    for (let i = 0; i < layers; i++) {
        const t = i / Math.max(1, layers - 1);
        // Birth window: layer i grows in over [i/N, (i+1)/N] of age01.
        const birth = i / layers;
        const death = (i + 1) / layers;
        const grow = smoothstep(birth, death, age01);
        if (grow <= 0) continue;

        const y = baseY + t * span;
        const layerR = (baseCanopyRadius * (1 - t * 0.85) + tipR * t) *
                       Math.max(0.2, age01) * grow;
        const layerHeight = layerH * (0.7 + 0.3 * (1 - t)) * grow;

        const cone = buildCone(layerR, layerHeight, 12, 2);
        cone.translate(0, y, 0);
        coneParts.push({ mesh: cone, color: canopyColor, metallic: 0, roughness: 0.9 });
        aabbInclude(aabb, [-layerR, y, -layerR]);
        aabbInclude(aabb, [layerR, y + layerHeight, layerR]);
        const top = y + layerHeight;
        if (top > canopyTopY) canopyTopY = top;
    }

    // Trunk height = a touch above the highest grown cone so the apex is
    // never a bare spike past the foliage. Falls back to a small stub
    // when no cones have grown yet (very young age).
    const trunkH = Math.max(Heff * 0.18, canopyTopY * 0.95);
    const trunkMesh = buildCone(trunkRadius, trunkH, 12, 1);
    parts.push({
        mesh: trunkMesh,
        color: COLORS.bark,
        metallic: 0,
        roughness: 0.95,
    });
    aabbInclude(aabb, [-trunkRadius, 0, -trunkRadius]);
    aabbInclude(aabb, [trunkRadius, trunkH, trunkRadius]);

    for (const c of coneParts) parts.push(c);

    return { parts, aabbMin: aabb.min, aabbMax: aabb.max };
}

// ─── Recipe: shrub ────────────────────────────────────────────────────────
// Several small noise-displaced blobs clumped low. Optional thin stems
// poking out the bottom for "twig at the base" character.

function shrub(opts) {
    opts = opts || {};
    const seed = (opts.seed | 0) || 1;
    const age01 = Math.max(0.05, Math.min(1, opts.age01 ?? 1));
    const H = opts.height ?? 1.5;
    const R = opts.radius ?? 1.2;
    const blobCount = Math.max(2, opts.blobCount ?? 5);
    const canopyColor = opts.canopyColor || COLORS.canopyShrub;

    const Heff = H * age01;
    const Reff = R * Math.max(0.4, age01 * 0.7 + 0.3);

    const rng = mulberry32(seed);
    const parts = [];
    const aabb = emptyAabb();

    for (let i = 0; i < blobCount; i++) {
        // Distribute blobs in a flattened disc-like pattern: angular
        // jittering plus radial offset, low height variance.
        const a = TAU * i / blobCount + rng() * 0.5;
        const off = Reff * (0.2 + rng() * 0.55);
        const c = [
            Math.cos(a) * off,
            Heff * (0.35 + rng() * 0.4),
            Math.sin(a) * off,
        ];
        const r = Reff * (0.40 + rng() * 0.25);
        const blob = buildBlob(c, r, (seed * 13 + i * 29) ^ 0x3003,
            { nsub: 2, sy: 0.85 });
        parts.push({
            mesh: blob, color: canopyColor,
            metallic: 0, roughness: 0.88,
        });
        aabbInclude(aabb, c, r * 1.1);
    }

    return { parts, aabbMin: aabb.min, aabbMax: aabb.max };
}

// ─── Recipe: vine ─────────────────────────────────────────────────────────
// Helix sweep + small leaf-blobs spaced along the path.

function vine(opts) {
    opts = opts || {};
    const seed = (opts.seed | 0) || 1;
    const age01 = Math.max(0.05, Math.min(1, opts.age01 ?? 1));
    const length = (opts.length ?? 6) * age01;
    const radius = opts.radius ?? 0.04;
    const helixRadius = opts.helixRadius ?? 0.5;
    const turns = opts.turns ?? 3;
    const canopyColor = opts.canopyColor || COLORS.canopyVine;

    const rng = mulberry32(seed);
    const samples = Math.max(16, Math.floor(length * 12));
    const path = [];
    for (let i = 0; i <= samples; i++) {
        const t = i / samples;
        const a = t * turns * TAU;
        path.push([
            Math.cos(a) * helixRadius + (rng() - 0.5) * 0.04,
            t * length,
            Math.sin(a) * helixRadius + (rng() - 0.5) * 0.04,
        ]);
    }
    const profile = circleProfile(6, 1);
    const profileScale = new Array(path.length).fill(radius);
    const stem = Mesh.sweep(profile, path, {
        closeProfile: true, capStart: true, capEnd: true,
        miterJoints: true, profileScale,
    });

    const parts = [];
    if (stem) parts.push({
        mesh: stem, color: COLORS.barkLight, metallic: 0, roughness: 0.9,
    });

    // Leaf blobs at intervals along the helix.
    const leafEvery = Math.max(1, Math.floor(samples / Math.max(8, length * 4)));
    const blobR = Math.max(0.06, helixRadius * 0.45);
    for (let i = 0; i < path.length; i += leafEvery) {
        const c = path[i];
        const blob = buildBlob(c, blobR * (0.85 + rng() * 0.35),
            (seed * 7 + i * 41) ^ 0x4004, { nsub: 1, sy: 0.7 });
        parts.push({
            mesh: blob, color: canopyColor, metallic: 0, roughness: 0.88,
        });
    }

    const aabb = emptyAabb();
    for (const p of path) aabbInclude(aabb, p, blobR * 1.5);
    return { parts, aabbMin: aabb.min, aabbMax: aabb.max };
}

// ─── Recipe: fern ─────────────────────────────────────────────────────────
// Parametric: a curving rachis with paired leaflets along it. Geometry-only.
// Already reads as fern; keep as-is, just wrap into the new {parts} shape.

function fern(opts) {
    opts = opts || {};
    const pairs = Math.max(2, opts.leafletPairs ?? 14);
    const age01 = Math.max(0.05, Math.min(1, opts.age01 ?? 1));
    const length = (opts.length ?? 1.5) * age01;
    const stemRadius = opts.stemRadius ?? 0.012;
    const leafletLength = opts.leafletLength ?? 0.32;
    const curvature = opts.curvature ?? 1.4;

    const fCount = pairs;
    const step = length / fCount;
    const bendPerStep = curvature / fCount;
    let dir = [0, 1, 0];
    let cur = [0, 0, 0];
    const rachis = [cur];
    for (let i = 0; i < fCount; i++) {
        const c = Math.cos(bendPerStep), s = Math.sin(bendPerStep);
        dir = vNorm([dir[0], c*dir[1] - s*dir[2], s*dir[1] + c*dir[2]]);
        cur = vAdd(cur, vScale(dir, step));
        rachis.push(cur);
    }

    const sub = [];
    const stemProfile = circleProfile(6, 1);
    const stemScale = rachis.map((_, i) => {
        const t = i / (rachis.length - 1);
        return stemRadius * Math.max(0.15, 1 - t);
    });
    const rachisMesh = Mesh.sweep(stemProfile, rachis, {
        closeProfile: true, capStart: true, capEnd: true,
        miterJoints: true, profileScale: stemScale,
    });
    if (rachisMesh) sub.push(rachisMesh);

    for (let i = 1; i < rachis.length; i++) {
        const p = rachis[i];
        const tangent = vNormOr(vSub(rachis[i], rachis[i - 1]), [0, 1, 0]);
        const t = i / (rachis.length - 1);
        const taper = Math.sin(Math.PI * t);
        const ll = leafletLength * Math.max(0.15, taper);

        let ortho = vCross(tangent, [0, 0, 1]);
        if (vLen(ortho) < 0.5) ortho = vCross(tangent, [1, 0, 0]);
        ortho = vNorm(ortho);

        for (const sign of [1, -1]) {
            const sideDir = vScale(ortho, sign);
            const leafPath = [];
            const lsegs = 6;
            for (let k = 0; k <= lsegs; k++) {
                const u = k / lsegs;
                leafPath.push(vAdd(vAdd(p, vScale(sideDir, u * ll)),
                                          vScale(tangent, u * ll * 0.1)));
            }
            const leafW = 0.025 * Math.max(0.2, taper);
            const leafProfile = [
                [leafW, 0], [0, leafW * 0.2],
                [-leafW, 0], [0, -leafW * 0.2],
            ];
            const leafScale = leafPath.map((_, k) => {
                const u = k / (leafPath.length - 1);
                return Math.max(0.1, 1 - u);
            });
            const lm = Mesh.sweep(leafProfile, leafPath, {
                closeProfile: true, capStart: false, capEnd: true,
                miterJoints: false, profileScale: leafScale,
            });
            if (lm) sub.push(lm);
        }
    }

    const merged = sub.length > 1 ? Mesh.merge(sub) : sub[0];
    const aabb = emptyAabb();
    for (const p of rachis) aabbInclude(aabb, p, leafletLength);

    return {
        parts: [{ mesh: merged, color: COLORS.fernLeaf, metallic: 0, roughness: 0.9 }],
        aabbMin: aabb.min, aabbMax: aabb.max,
    };
}

// ─── Recipe: grassTuft ────────────────────────────────────────────────────

function grassTuft(opts) {
    opts = opts || {};
    const seed = (opts.seed | 0) || 1;
    const age01 = Math.max(0.05, Math.min(1, opts.age01 ?? 1));
    const blades = Math.max(1, opts.bladeCount ?? 12);
    const height = opts.height ?? 0.4;
    const baseRadius = opts.baseRadius ?? 0.08;
    const bladeWidth = opts.bladeWidth ?? 0.012;
    const bend = opts.bend ?? 0.6;

    const rng = mulberry32(seed);
    const sub = [];
    for (let i = 0; i < blades; i++) {
        const ang = TAU * i / blades + rng() * 0.4;
        const br = baseRadius * (0.4 + 0.6 * rng());
        const base = [Math.cos(ang) * br, 0, Math.sin(ang) * br];
        const bladeH = height * age01 * (0.7 + 0.6 * rng());
        const tipBend = bend * (0.6 + 0.8 * rng());
        const outDir = [Math.cos(ang), 0, Math.sin(ang)];

        const path = [];
        const segs = 8;
        for (let s = 0; s <= segs; s++) {
            const t = s / segs;
            const lateral = Math.sin(t * tipBend) * bladeH * 0.35;
            const vertical = Math.cos(t * tipBend) * bladeH * t;
            path.push([
                base[0] + outDir[0] * lateral,
                base[1] + vertical,
                base[2] + outDir[2] * lateral,
            ]);
        }
        const profile = [
            [bladeWidth, 0], [0, bladeWidth * 0.15],
            [-bladeWidth, 0], [0, -bladeWidth * 0.15],
        ];
        const profileScale = path.map((_, s) => {
            const t = s / (path.length - 1);
            return Math.max(0.05, 1 - t);
        });
        const twist = path.map(() => -ang);
        const blade = Mesh.sweep(profile, path, {
            closeProfile: true, capStart: false, capEnd: true,
            miterJoints: true, profileScale, twist,
        });
        if (blade) sub.push(blade);
    }

    const merged = sub.length > 1 ? Mesh.merge(sub) : sub[0];
    const aabb = emptyAabb();
    aabbInclude(aabb, [-baseRadius, 0, -baseRadius]);
    aabbInclude(aabb, [baseRadius, height * age01, baseRadius]);

    return {
        parts: [{ mesh: merged, color: COLORS.grassBlade, metallic: 0, roughness: 0.95 }],
        aabbMin: aabb.min, aabbMax: aabb.max,
    };
}

// ─── Recipe: succulent ────────────────────────────────────────────────────

function succulent(opts) {
    opts = opts || {};
    const seed = (opts.seed | 0) || 1;
    const age01 = Math.max(0.05, Math.min(1, opts.age01 ?? 1));
    const leafCount = Math.max(3, opts.leafCount ?? 24);
    const leafLength = opts.leafLength ?? 0.35;
    const leafWidth = opts.leafWidth ?? 0.06;
    const leafThickness = opts.leafThickness ?? 0.02;
    const tilt = opts.tilt ?? 0.6;

    const rng = mulberry32(seed);
    const golden = 2.39996323;
    const sub = [];
    for (let i = 0; i < leafCount; i++) {
        const a = i * golden;
        const t = i / leafCount;
        const lt = tilt * (0.4 + 0.7 * t);
        const len = leafLength * age01 * (0.7 + 0.4 * t + rng() * 0.1);
        const outDir = vNorm([
            Math.cos(a) * Math.cos(lt),
            Math.sin(lt),
            Math.sin(a) * Math.cos(lt),
        ]);
        const path = [];
        const segs = 8;
        for (let s = 0; s <= segs; s++) {
            const u = s / segs;
            const p = vScale(outDir, u * len);
            p[1] += u * u * len * 0.25;
            path.push(p);
        }
        const profile = [
            [leafWidth, 0], [0, leafThickness],
            [-leafWidth, 0], [0, -leafThickness],
        ];
        const profileScale = path.map((_, s) => {
            const u = s / (path.length - 1);
            const bulge = Math.sin(Math.PI * u);
            return Math.max(0.05, 0.4 + 0.6 * bulge - 0.5 * u);
        });
        const leaf = Mesh.sweep(profile, path, {
            closeProfile: true, capStart: true, capEnd: true,
            miterJoints: true, profileScale,
        });
        if (leaf) sub.push(leaf);
    }

    const merged = sub.length > 1 ? Mesh.merge(sub) : sub[0];
    const aabb = emptyAabb();
    aabbInclude(aabb, [-leafLength, 0, -leafLength]);
    aabbInclude(aabb, [leafLength, leafLength, leafLength]);

    return {
        parts: [{ mesh: merged, color: COLORS.succulent, metallic: 0, roughness: 0.7 }],
        aabbMin: aabb.min, aabbMax: aabb.max,
    };
}

// ─── Export ───────────────────────────────────────────────────────────────

root.Recipes = {
    tree, conifer, shrub, vine, fern, grassTuft, succulent,
    CANOPY_SHAPES,
};

})(this);
