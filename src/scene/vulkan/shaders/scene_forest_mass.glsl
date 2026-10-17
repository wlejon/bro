// Leaves placed by the fragment shader (scene/forest_node.h): a card on
// which each pixel finds the leaves that cover it, so a crown is drawn as
// leaves of the leaf's own size, count and turn without a vertex a leaf and
// without anything baked from the tree. Used by mesh.frag for
//   - a forest LOBE (mesh_forest_sprite.vert, SCENE_FOREST_SPRITE): one of
//     the three spheres a clump's leaves fill (procClumpLobe), on a card
//     facing the eye. Its leaves lie in a shell under the sphere, facing out
//     of it (clumpLeafFrame), so the eye sees the near half's leaves facing
//     it and, through their gaps, the far half's from behind (as their
//     mean); the crown tier's one sphere a tree is drawn the same way.
//   - a leaf CLUSTER (mesh_forest_leaf.vert, SCENE_FOREST_CLUSTER): a
//     thinned crown's widened leaf card, drawn as the leaves it stands for.
//
// The card is split into square cells, a leaf in each, at a jittered point
// (the leaves' count over the area sets the cell). A leaf is an ellipse seen
// on the card (its length and width along its own axes, foreshortened by its
// turn), PREFILTERED to the pixel: each ellipse is widened by the pixel's
// footprint and its cover scaled down by the area it gained, so leaves
// smaller than a pixel resolve to their mean cover and colour rather than
// sparkling, and a leaf of several pixels has a sharp, antialiased edge.
// Once a pixel spans a cell the lobe's leaves are taken as their mean.

// The optical depth of a sphere's leaves through its centre: its leaves
// (two a cell squared over the disc's area, `cell` in radii), each an
// ellipse of half axes a and a times `aspect` (a half a leaf's length `len`,
// in radii) turned every way (about 0.55 of its area faces any direction).
// The light through the middle is exp(-it), and a chord's in proportion.
float forestMassDepth(float cell, float len, float aspect) {
    float leaves = 6.2831853 / max(cell * cell, 1e-6);
    float a = 0.5 * len;
    return leaves * a * a * aspect * 0.55;
}

// Four values in [0, 1] from a cell and a salt (an integer hash).
vec4 forestMassHash(ivec2 c, uint salt) {
    uint v = uint(c.x + 32768) * 73856093u ^ uint(c.y + 32768) * 19349663u ^ salt * 83492791u;
    v = (v ^ (v >> 16u)) * 0x7feb352du;
    v = (v ^ (v >> 15u)) * 0x846ca68bu;
    v ^= v >> 16u;
    return vec4(uvec4(v & 255u, (v >> 8u) & 255u, (v >> 16u) & 255u, v >> 24u)) / 255.0;
}

// A unit vector perpendicular to unit `n`.
vec3 forestMassPerp(vec3 n) {
    return normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
}

// The share of a pixel (footprint `fp` across, card units) the leaf centred
// at `at` covers: its half axes on the card `a1` (along it) and `a2`
// (across), as an ellipse's shape matrix widened by the pixel.
float forestLeafCover(vec2 q, vec2 at, vec2 a1, vec2 a2, float fp) {
    float sxx = a1.x * a1.x + a2.x * a2.x, syy = a1.y * a1.y + a2.y * a2.y, sxy = a1.x * a1.y + a2.x * a2.y;
    float area = max(sxx * syy - sxy * sxy, 1e-12);
    // A pixel's spread (a box's variance) in the same terms.
    float pix = fp * fp * (1.0 / 3.0);
    sxx += pix;
    syy += pix;
    float det = sxx * syy - sxy * sxy;
    vec2 d = q - at;
    vec2 id = vec2(syy * d.x - sxy * d.y, sxx * d.y - sxy * d.x) / det;
    float e = dot(d, id);
    // The edge's slope in pixels: e is 1 there and grows by about 2|id| fp a
    // pixel.
    float g = max(2.0 * length(id) * fp, 1e-6);
    float edge = clamp((1.0 - e) / g + 0.5, 0.0, 1.0);
    return edge * sqrt(area / det);
}

// One layer of leaves at card point `q` (footprint `fp`).
struct ForestLeafLayer {
    float clear;    // the share of the pixel no leaf covers
    vec3 normal;    // the visible leaves' mean normal, card basis (z toward the eye), not normalised
    float under;    // their share showing their undersides (weighted as normal)
    float tone;     // their own lightness, weighted
    float z;        // their depth toward the eye (sphere radii from the centre), weighted
    float weight;   // the sum of the weights
    // The leaf that shows most: its own draw in [0, 1) for whether the sun
    // reaches it (SceneSurface::leafSun), and its weight.
    float sun;
    float sunWeight;
};

// Takes a leaf of normal `n` (toward the eye), cover `cov`, weight `w`, depth
// `z` and sun draw `sun` into the layer.
void forestLeafAdd(inout ForestLeafLayer o, vec3 n, float cov, float w, float tone, float z, float sun) {
    o.clear *= 1.0 - cov;
    bool under = n.z < 0.0;
    vec3 f = under ? -n : n;
    o.normal += f * w;
    o.under += under ? w : 0.0;
    o.tone += tone * w;
    o.z += z * w;
    o.weight += w;
    if (w > o.sunWeight) {
        o.sunWeight = w;
        o.sun = sun;
    }
}

ForestLeafLayer forestLeafLayerNone() {
    ForestLeafLayer o;
    o.clear = 1.0;
    o.normal = vec3(0.0);
    o.under = o.tone = o.z = o.weight = 0.0;
    o.sun = 0.5;
    o.sunWeight = 0.0;
    return o;
}

// The leaves on one half of a unit sphere's shell seen on the card (a LOBE):
// `side` 1 the near half, -1 the far. `cell` the cell size, `len` a leaf's
// length (both in sphere radii), `aspect` its width over length, `shell` the
// depth of the shell they fill (clumpLeafFrame's), `upC` the world's up in
// card terms, `upBias` / `tilt` how they turn (the scatter's), `salt` the
// lobe's.
ForestLeafLayer forestLobeLayer(vec2 q, float fp, float side, float cell, float len, float aspect, float shell,
                                vec3 upC, float upBias, float tilt, uint salt) {
    ForestLeafLayer o = forestLeafLayerNone();
    ivec2 base = ivec2(floor(q / cell));
    // A leaf reaches no further from its centre than its largest half
    // length, widened by the pixel.
    float reach = 0.6 * len + fp;
    for (int dj = -1; dj <= 1; ++dj) {
        for (int di = -1; di <= 1; ++di) {
            ivec2 c = base + ivec2(di, dj);
            vec4 h = forestMassHash(c, salt);
            vec2 at = (vec2(c) + 0.5 + (h.xy - 0.5) * 0.9) * cell;
            if (dot(q - at, q - at) > reach * reach) continue;
            // Its depth in the shell, and its place on the sphere there.
            float rr = 1.0 - shell * h.z;
            float r2 = dot(at, at) / (rr * rr);
            if (r2 >= 1.0) continue;
            vec4 g = forestMassHash(c, salt ^ 0x5bd1e995u);
            vec3 u = vec3(at / rr, side * sqrt(1.0 - r2));
            vec3 n = normalize(u * (1.0 - upBias) + upC * upBias + (g.xyz * 2.0 - 1.0) * tilt);
            vec3 t = forestMassPerp(n);
            float th = 6.2831853 * h.w;
            t = t * cos(th) + cross(n, t) * sin(th);
            vec3 b = cross(n, t);
            float size = 0.5 * len * (0.8 + 0.4 * g.w);
            float cov = forestLeafCover(q, at, t.xy * size, b.xy * (size * aspect), fp);
            if (cov <= 0.0) continue;
            // The nearer leaves of the layer show over the further.
            float sun = forestMassHash(c, salt ^ 0x68e31da4u).x;
            forestLeafAdd(o, n, cov, cov * exp2(4.0 * side * u.z), 0.9 + 0.2 * fract(g.w * 7.31 + h.z), rr * u.z, sun);
        }
    }
    return o;
}

// The same layer once a pixel holds several leaves: their mean, a leaf a
// cell squared each covering its turned area, facing out of the shell's
// middle (what the leaves resolve to there, at a fraction of the cost).
// The mean of a pixel's few leaves is not the crown's mean: how many there
// are and which way they face wander from pixel to pixel by about one over
// the root of their count. forestMassGrain gives that wander, in squares
// of a pixel or so on the cells' own grid (2^k cells on a side, blended
// between the two octaves either side of the pixel), so it holds still
// as the crown moves and grows no finer than the pixel as it recedes.
vec4 forestMassGrain(vec2 q, float fp, float cell, uint salt) {
    float lv = max(log2(fp / cell), 0.0);
    float k0 = floor(lv), f = lv - k0;
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 2; ++i) {
        float k = k0 + float(i);
        float S = cell * exp2(k);
        vec4 g = forestMassHash(ivec2(floor(q / S)), salt ^ (uint(k + 1.0) * 0x9e3779b9u)) * 2.0 - 1.0;
        sum += g * exp2(-k) * (i == 0 ? 1.0 - f : f);
    }
    return sum;
}

// `g` the pixel's grain (forestMassGrain).
ForestLeafLayer forestLobeMean(vec2 q, float side, float cell, float len, float aspect, float shell, vec3 upC,
                               float upBias, vec4 g) {
    ForestLeafLayer o = forestLeafLayerNone();
    float rr = 1.0 - 0.5 * shell;
    float r2 = dot(q, q) / (rr * rr);
    if (r2 >= 1.0) return o;
    float a = 0.5 * len;
    // Uniform in -1..1 has a deviation of 0.58: the count's relative wander
    // (Poisson, 1/sqrt(n)) is the grain times 1.7, the facing's about the
    // spread of one leaf's from the shell's (0.5) over the same root.
    float tau = 3.14159265 * a * a * aspect * 0.55 / (cell * cell) * max(1.0 + 1.7 * g.w, 0.0);
    float cov = 1.0 - exp(-tau);
    vec3 u = vec3(q / rr, side * sqrt(1.0 - r2));
    vec3 n = normalize(u * (1.0 - upBias) + upC * upBias + g.xyz * 0.85);
    forestLeafAdd(o, n, cov, cov * exp2(4.0 * side * u.z), 1.0 + 0.1 * g.x * g.y, rr * u.z, 0.5);
    return o;
}

// A lobe's layer at the cost its pixel needs: leaf by leaf while a leaf
// spans about a pixel or more, their mean once a pixel spans a cell, and a
// blend between (forestLobeMeanShare). `g` the pixel's grain.
float forestLobeMeanShare(float fp, float cell) {
    return smoothstep(0.7, 1.4, fp / cell);
}

ForestLeafLayer forestLobeLayerAt(vec2 q, float fp, float side, float cell, float len, float aspect, float shell,
                                  vec3 upC, float upBias, float tilt, uint salt, vec4 g) {
    float t = forestLobeMeanShare(fp, cell);
    if (t <= 0.0) return forestLobeLayer(q, fp, side, cell, len, aspect, shell, upC, upBias, tilt, salt);
    ForestLeafLayer m = forestLobeMean(q, side, cell, len, aspect, shell, upC, upBias, g);
    if (t >= 1.0) return m;
    ForestLeafLayer e = forestLobeLayer(q, fp, side, cell, len, aspect, shell, upC, upBias, tilt, salt);
    e.clear = mix(e.clear, m.clear, t);
    e.normal = mix(e.normal, m.normal, t);
    e.under = mix(e.under, m.under, t);
    e.tone = mix(e.tone, m.tone, t);
    e.z = mix(e.z, m.z, t);
    e.weight = mix(e.weight, m.weight, t);
    return e;
}

// A CLUSTER: a thinned crown's kept leaf, widened k times to stand for the
// k^2 leaves round it, drawn as those leaves, each of its own size, inside
// the widened outline: the kept one in the middle and the rest a cell apart
// in its plane, each turned and tipped a little from it. `p` the point in
// leaf lengths from the outline's middle (x across, y along), `k` the
// widening (under 1 the kept leaf shrinking away), `aspect` the leaf's
// width over its length. The card's terms are x across, y along, z toward
// the eye: `keptZ` the kept leaf's normal there (1 facing the eye, -1 away),
// `lobe` the way the leaves round it face before their tilt (unit), `tilt`
// that tilt (clumpLeafFrame's). The normal it returns is in those terms.
ForestLeafLayer forestClusterLayer(vec2 p, float fp, float k, float aspect, float keptZ, vec3 lobe, float tilt,
                                   uint salt) {
    ForestLeafLayer o = forestLeafLayerNone();
    // A leaf's area of outline to each (a leaf is about an ellipse).
    float cell = sqrt(0.785 * aspect);
    vec2 half_ = vec2(0.5 * aspect, 0.5) * max(k, 1e-3);
    // The four cells whose centres surround p: a leaf is jittered at most
    // 0.175 cell, so any other's is over 0.82 cell off, past the largest
    // leaf's half length (0.55).
    ivec2 base = ivec2(floor(p / cell));
    for (int dj = 0; dj <= 1; ++dj) {
        for (int di = 0; di <= 1; ++di) {
            ivec2 c = base + ivec2(di, dj);
            bool kept = c.x == 0 && c.y == 0;
            vec4 h = forestMassHash(c, salt);
            vec2 at = kept ? vec2(0.0) : (vec2(c) + (h.xy - 0.5) * 0.35) * cell;
            if (dot(p - at, p - at) > (0.55 + fp) * (0.55 + fp)) continue;
            // A leaf joins as the outline widens past it, growing in, so a
            // cluster widening with distance never pops.
            float size = kept ? min(k, 1.0) : clamp((0.95 - length(at / half_)) * 4.0, 0.0, 1.0);
            if (size <= 0.0) continue;
            vec4 g = forestMassHash(c, salt ^ 0x5bd1e995u);
            float th = kept ? 0.0 : (h.w - 0.5) * 1.4;
            vec2 along = vec2(sin(th), cos(th));
            size *= 0.5 * (kept ? 1.0 : 0.9 + 0.2 * g.w);
            float cov = forestLeafCover(p, at, along * size, vec2(along.y, -along.x) * (size * aspect), fp);
            if (cov <= 0.0) continue;
            // Each of the others faces its own way, as the leaves it stands
            // for: out of the lobe, leaning up, tilted.
            vec4 g2 = forestMassHash(c, salt ^ 0x27d4eb2fu);
            vec3 n = kept ? vec3(0.0, 0.0, keptZ) : lobe + (g2.xyz * 2.0 - 1.0) * tilt;
            n = dot(n, n) > 1e-8 ? normalize(n) : vec3(0.0, 0.0, keptZ);
            // Each a little before or behind the kept leaf (leaf lengths).
            float z = kept ? 0.0 : (fract(g.z * 3.7 + h.w * 1.9) - 0.5) * 0.6 * k;
            forestLeafAdd(o, n, cov, cov * (kept ? 1.5 : 0.5 + h.z), kept ? 1.0 : 0.9 + 0.2 * g.z, z,
                          fract(g.w * 5.77 + h.z * 3.13));
        }
    }
    return o;
}

// The crown's shading at a leaf at `rc` (its offset from the crown's centre
// over the crown's radius), as vegCrownShade and vegetationTint have it: the
// bulk normal (unit), the ambient occlusion, and the colour's depth term.
void forestCrownTerms(vec3 rc, out vec3 bulk, out float ao, out vec3 deep) {
    bulk = normalize(rc + vec3(0.0, 0.25, 0.0));
    float depth = clamp(length(rc), 0.0, 1.2);
    ao = clamp(0.25 + 0.85 * depth * depth, 0.25, 1.0) * mix(0.75, 1.0, clamp(rc.y * 0.5 + 0.6, 0.0, 1.0));
    deep = mix(vec3(0.5, 0.62, 0.68), vec3(1.0), smoothstep(0.25, 1.0, depth)) *
           (1.0 + 0.15 * smoothstep(0.85, 1.15, depth));
}

// A broad forest leaf cut from its card per pixel (MESH_FOREST_LEAF_CUT,
// mesh_forest_leaf.vert): `uv` x across the card (0.5 the midrib, 0 and 1
// its widest), y along it (1 at the stalk, 0 the tip); `shape` its species'
// leaf (broflora's Species::leafWidth .. leafGloss): its width as a share of
// the card's, its tip (0 drawn to a point, 1 rounded), its teeth a side plus
// their depth (the fraction, of the half-width) and its gloss. Everything is
// worked out from where the pixel lies, so it costs no geometry, and each
// detail fades out as it shrinks under a couple of pixels. Fragment stages
// only (it takes derivatives): mesh.frag defines FOREST_MASS_FRAGMENT.
#ifdef FOREST_MASS_FRAGMENT
struct ForestLeafCut {
    float cover;   // the share of the pixel the leaf covers
    float vein;    // 0..1: on the midrib or a lateral vein
    float tilt;    // a wavy margin's lean of the normal along the leaf
    float gloss;   // 0 matte .. 1 waxy
};

ForestLeafCut forestLeafCut(vec2 uv, vec4 shape) {
    ForestLeafCut c;
    float across = abs(uv.x * 2.0 - 1.0);
    c.gloss = clamp(shape.w, 0.0, 1.0);
    // A leaf a few pixels across shows no outline, only its area: it hands
    // over to the whole card, the area the far tiers (clusters, lobes,
    // crowns) stand in for, so a crown seen past the cut holds its cover.
    // (Coarse: one answer a quad, so the far leaves skip the cut whole.)
    float far = smoothstep(0.25, 0.5, fwidthCoarse(across));
    if (far >= 1.0) {
        c.cover = 1.0;
        c.vein = 0.0;
        c.tilt = 0.0;
        return c;
    }
    float t = clamp(1.0 - uv.y, 0.0, 1.0);
    float width = shape.x > 0.0 ? shape.x : 1.0;
    float tip = clamp(shape.y, 0.0, 1.0);
    float teeth = floor(shape.z);
    float depth = shape.z - teeth;
    // The outline: widest a little below the middle, and toward the tip
    // drawn to a point (acuminate) or rounded (obtuse).
    float s = max(sin(3.14159265 * pow(t, 0.85)), 0.0);
    float k = mix(0.75, mix(1.15, 0.45, tip), smoothstep(0.45, 0.95, t));
    float edge = 0.92 * width * pow(s, k);
    // Teeth along each side, from just above the base to the tip: a
    // serration (each tooth's long edge rising toward the tip, its short
    // one dropping back) or, deep, spines (sharp points between concave
    // scallops, one at the tip), whose margin waves up and down.
    c.tilt = 0.0;
    if (teeth > 0.5 && depth > 0.0) {
        float m = clamp((t - 0.12) / 0.86, 0.0, 1.0);
        float ph = fract(m * teeth);
        float spiny = smoothstep(0.15, 0.3, depth);
        float g = mix(1.0 - ph, pow(sin(3.14159265 * ph), 0.6), spiny);
        float fade = smoothstep(0.0, 0.1, m) * mix(1.0 - smoothstep(0.85, 1.0, m), 1.0, spiny);
        edge *= 1.0 - depth * g * fade;
        c.tilt = 0.9 * spiny * depth * sin(6.2831853 * m * teeth) * smoothstep(0.3, 0.9, across / max(edge, 1e-3));
    }
    float fpx = max(fwidth(across) + 0.5 * fwidth(edge), 1e-5);
    c.cover = clamp((edge - across) / fpx + 0.5, 0.0, 1.0);
    c.cover = mix(c.cover, 1.0, far);
    // Venation: the midrib, tapering to the tip, and pinnate laterals
    // leaving it for the margin at a slant toward the tip, curving up
    // along it as they near it.
    float fa = fwidth(across);
    float rib = 0.035 * (1.0 - 0.6 * t);
    float mid = (1.0 - smoothstep(rib, rib + fa, across)) * clamp((0.05 - fa) / 0.03, 0.0, 1.0);
    float u = 7.0 * (t - 0.18 * across - 0.22 * across * across);
    float f = fract(u);
    float fu = fwidth(u);
    float lat = (1.0 - smoothstep(0.05, 0.05 + fu, min(f, 1.0 - f))) * (1.0 - smoothstep(0.1, 0.25, fu)) *
                smoothstep(0.08, 0.25, t) * (1.0 - smoothstep(0.75, 0.95, across / max(edge, 1e-3)));
    c.vein = max(mid, 0.45 * lat);
    return c;
}
#endif
