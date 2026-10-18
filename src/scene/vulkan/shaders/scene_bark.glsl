// Bark: the wood of a plant's tubes (mesh_tube.vert, mesh_forest_tube.vert)
// drawn as bark rather than clay. Everything is procedural and comes from
// the tube's own records, so nothing is baked per tree:
//
//   - the vertex stage places each point on the bark: where it lies across
//     the stem (the ring's radial direction times the radius, in metres),
//     how far along the wood it is from the plant's root (the records' path,
//     broflora SegmentSway::pathFrom, so the pattern runs on unbroken
//     through joints), the stem's radius there, its height over the root
//     and the plant's bark word (broflora packBarkWord: furrow, scale and
//     moss). It also splits the trunk's foot into buttresses.
//   - the fragment stage (barkSurface) cuts ridges and furrows that run
//     along the stem (noise stretched along it; the furrow is its middle
//     value), spaced wider as the girth grows, with a relief the normal
//     follows (the noise's analytic gradient, so it is smooth per pixel);
//     it darkens the furrows, greys the ridges' tops, mottles the colour,
//     and lays moss and lichen where they grow: the shaded side, the upper
//     side of a limb and the foot of the trunk. Detail finer than about two
//     pixels fades to its mean, so far bark does not shimmer.
//
// Needs scene_camera.glsl.

// The side moss favours: away from a sun in the south (bro's skies put the
// sun toward +Z, its light travelling to -Z), so facing -Z.
const vec3 BARK_MOSS_SIDE = vec3(0.0, 0.0, -1.0);

// The plant's bark word (broflora packBarkWord): furrow 0..1, scale (x the
// default spacing) and moss 0..1; a field of code 0 takes the default.
void barkUnpack(float word, out float furrow, out float scale, out float moss) {
    uint w = uint(max(word, 0.0) + 0.5);
    uint f = w & 255u, s = (w >> 8) & 255u, m = (w >> 16) & 255u;
    furrow = f == 0u ? 0.55 : float(f - 1u) / 254.0;
    scale = s == 0u ? 1.0 : 0.25 * pow(16.0, float(s - 1u) / 254.0);
    moss = m == 0u ? 0.5 : float(m - 1u) / 254.0;
}

// The buttresses of a trunk's foot: how far (x the radius) the point at
// angle `ang` round the stem stands out, `hb` above the root, the ring's
// radius `rad`. Five lobes, a little irregular and turned by the tree's
// `seed`, dying away within about a radius of the ground.
float barkButtress(float ang, float hb, float rad, float seed) {
    float k = exp(-max(hb, 0.0) / max(0.9 * rad, 1e-4));
    if (k < 0.01) return 0.0;
    float c = 0.5 + 0.5 * cos(5.0 * ang + seed * 6.2831853 + 0.8 * sin(3.0 * ang + seed * 17.0));
    return 0.45 * k * (c * c - 0.3);
}

// The fragment half: a vertex stage defines SCENE_BARK_VERTEX before the
// include to leave it out (it takes screen derivatives).
#ifndef SCENE_BARK_VERTEX

// An integer hash of a lattice point into [0, 1).
float barkHash(ivec3 p) {
    uint h = uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(p.z) * 0xcb1ab31fu;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// Value noise in [0, 1) (quintic), and its gradient in `d`.
float barkNoiseD(vec3 p, out vec3 d) {
    vec3 i = floor(p), f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    ivec3 c = ivec3(i);
    float a = barkHash(c), b = barkHash(c + ivec3(1, 0, 0));
    float cc = barkHash(c + ivec3(0, 1, 0)), dd = barkHash(c + ivec3(1, 1, 0));
    float e = barkHash(c + ivec3(0, 0, 1)), ff = barkHash(c + ivec3(1, 0, 1));
    float g = barkHash(c + ivec3(0, 1, 1)), h = barkHash(c + ivec3(1, 1, 1));
    float k1 = b - a, k2 = cc - a, k3 = e - a, k4 = a - b - cc + dd;
    float k5 = a - cc - e + g, k6 = a - b - e + ff, k7 = -a + b + cc - dd + e - ff - g + h;
    d = du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                  k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                  k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
    return a + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x +
           k7 * u.x * u.y * u.z;
}

float barkNoise(vec3 p) {
    vec3 d;
    return barkNoiseD(p, d);
}

vec3 barkHash3(ivec3 p) {
    return vec3(barkHash(p), barkHash(p + ivec3(71, 13, 5)), barkHash(p + ivec3(29, 101, 47)));
}

// The bark's plates: a cell pattern in q (a jittered lattice, its cells the
// plates). Returns the gap to the nearest border (F2 - F1, 0 on a furrow)
// with its gradient in `d`, and in `cell` a hash of the plate it lies on.
float barkPlates(vec3 q, out vec3 d, out float cell) {
    ivec3 i = ivec3(floor(q));
    vec3 f = q - vec3(i);
    float d1 = 1e9, d2 = 1e9;
    vec3 v1 = vec3(0.0), v2 = vec3(0.0);
    float c1 = 0.0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                ivec3 o = ivec3(x, y, z);
                vec3 j = barkHash3(i + o);
                vec3 v = vec3(o) + 0.1 + 0.8 * j - f;
                float dd = dot(v, v);
                if (dd < d1) {
                    d2 = d1;
                    v2 = v1;
                    d1 = dd;
                    v1 = v;
                    c1 = j.x;
                } else if (dd < d2) {
                    d2 = dd;
                    v2 = v;
                }
            }
    d1 = sqrt(d1);
    d2 = sqrt(d2);
    // d|q - c| / dq = (q - c) / |q - c|, and v = c - q.
    d = -v2 / max(d2, 1e-5) + v1 / max(d1, 1e-5);
    cell = c1;
    return d2 - d1;
}

// The bark at a fragment. `coords` is the vertex stage's (across.xy, along,
// radius) in metres, `hb` its height over the root, `word` the bark word;
// `pos` the camera-relative position, `geomN` the surface normal and `axis`
// the stem's direction (up it). Updates the colour, the shading normal, the
// roughness and the ambient occlusion.
void barkSurface(vec4 coords, float hb, float word, vec3 pos, vec3 geomN, vec3 axis, inout vec3 color,
                 inout vec3 N, inout float roughness, inout float ao) {
    float furrow, scale, moss;
    barkUnpack(word, furrow, scale, moss);
    float r = max(coords.w, 1e-3);
    // A pale bark (a birch's white) comes with age: its twigs, under a
    // couple of centimetres across, are red-brown (not near black: deep in a
    // crown, in shade, wood that dark read as a cage of black rods).
    float pale = smoothstep(0.3, 0.55, dot(color, vec3(0.3, 0.5, 0.2)));
    color = mix(color, vec3(0.15, 0.095, 0.065), pale * (1.0 - smoothstep(0.006, 0.03, r)));
    // The ridges' spacing grows with the girth (young wood is smooth and
    // finely lined, an old trunk's ridges are fingers wide), and the relief
    // with it: twigs show none.
    float lambda = scale * (0.01 + 0.13 * r);
    float relief = furrow * smoothstep(0.006, 0.05, r);
    // How many pixels a ridge spans: detail under about two fades out.
    float fp = max(length(fwidth(pos)), 1e-5);
    float seen = clamp(lambda / fp * 0.5 - 0.5, 0.0, 1.0);
    float seenFine = clamp(lambda / fp * 0.2 - 0.6, 0.0, 1.0);
    // Streaks a few ridges wide, which outlast the ridges with distance.
    float seenCoarse = clamp(lambda / fp * 1.5 - 0.5, 0.0, 1.0);

    // Across the stem by where on its circle the point lies (scaled by the
    // radius there, so the pattern holds while the stem tapers), along it
    // stretched: furrows run with the wood. A slow warp makes them wander,
    // part and rejoin.
    // Plates: cells stretched along the stem, parted by furrows (their
    // borders) that wander, split and rejoin; fine fibres along the plates'
    // faces. Deep bark has narrow plates and wide furrows, smooth bark
    // broad faint ones.
    const float STRETCH = 6.0;
    vec3 q = vec3(coords.xy / lambda, coords.z / (lambda * STRETCH));
    float h = 0.75, cell = 0.5;
    vec3 dh = vec3(0.0);
    if (seen > 0.0) {
        vec3 wq = q * vec3(0.7, 0.7, 0.5);
        vec3 warp = vec3(barkNoise(wq + 11.3), barkNoise(wq + 27.1), 0.0) - 0.5;
        vec3 dg;
        float gap = barkPlates(q + 1.1 * warp, dg, cell);
        // Ragged edges: the gap wanders by a finer noise.
        vec3 dr;
        float rag = barkNoiseD(q * vec3(3.0, 3.0, 1.5) + 17.0, dr) - 0.5;
        gap += 0.18 * rag * seenFine;
        dg += 0.18 * seenFine * dr * vec3(3.0, 3.0, 1.5);
        // The furrow's width in q, never under about a pixel (so its edge
        // does not alias); the plate rounds over beyond it.
        float w = max(mix(0.08, 0.4, furrow), 1.2 * fp / lambda);
        float t = clamp(gap / w, 0.0, 1.0);
        float t2 = clamp(gap / (w + 0.6), 0.0, 1.0);
        float s1 = t * t * (3.0 - 2.0 * t), s2 = t2 * (2.0 - t2);
        float plate = s1 * (0.55 + 0.45 * s2);
        vec3 dPlate = ((gap < w ? 6.0 * t * (1.0 - t) / w : 0.0) * (0.55 + 0.45 * s2) +
                       s1 * 0.45 * (t2 < 1.0 ? (2.0 - 2.0 * t2) / (w + 0.6) : 0.0)) * dg;
        // Fibres: fine lines along the plates' faces.
        vec3 gf;
        float nf = barkNoiseD(q * vec3(4.0, 4.0, 0.6) + 5.7, gf);
        gf *= vec3(4.0, 4.0, 0.6) * seenFine;
        nf = mix(0.5, nf, seenFine);
        float hr = plate * (0.85 + 0.15 * nf + 0.1 * (cell - 0.5));
        vec3 dhr = dPlate * (0.85 + 0.15 * nf + 0.1 * (cell - 0.5)) + plate * 0.15 * gf;
        h = mix(0.75, hr, seen);
        dh = dhr * seen;
        cell = mix(0.5, cell, seen);
    }

    // The relief bends the normal: its gradient carried from the noise's
    // space onto the bark (around the stem and along it).
    vec3 T = normalize(axis - geomN * dot(axis, geomN));
    vec3 A = cross(T, geomN);
    vec2 dir = normalize(coords.xy + 1e-6);
    float dAround = dot(dh.xy, vec2(-dir.y, dir.x)) / lambda;
    float dAlong = dh.z / (lambda * STRETCH);
    float depth = relief * lambda * 0.15;
    N = normalize(N - depth * (A * dAround + T * dAlong));

    // Colour: furrows dark and warm, the plates' faces greyer and paler,
    // each plate its own tone, mottled in patches a few plates wide, and
    // finer flecks.
    float patchSize = 0.12 + 0.6 * r;
    float blotch = barkNoise(vec3(coords.xy, coords.z * 0.4) / patchSize + 3.1) * 0.7 +
                   barkNoise(vec3(coords.xy, coords.z * 0.4) * (2.7 / patchSize) + 13.7) * 0.3;
    // (Each pattern is only worked out where it can be seen.)
    float streak = seenCoarse > 0.0
        ? mix(0.5, barkNoise(vec3(coords.xy / (3.0 * lambda), coords.z / (30.0 * lambda)) + 7.7), seenCoarse)
        : 0.5;
    color *= 0.7 + 0.6 * streak;
    float fleck = seenFine > 0.0 ? mix(0.5, barkNoise(q * vec3(6.0, 6.0, 2.0) + 9.2), seenFine) : 0.5;
    // A bark of strong colour (a pine's red-orange plates) keeps it on its
    // plates' faces; a dull one weathers them grey.
    float grey = dot(color, vec3(0.3, 0.5, 0.2));
    float hiC = max(color.r, max(color.g, color.b));
    float sat = (hiC - min(color.r, min(color.g, color.b))) / max(hiC, 1e-4);
    vec3 face = mix(color, vec3(grey) * vec3(1.05, 1.02, 0.97), 0.65 * (1.0 - smoothstep(0.5, 0.8, sat))) * 1.3;
    vec3 c = mix(color * mix(1.0, 0.22, relief) * vec3(1.0, 0.85, 0.7), face, h);
    c *= 0.78 + 0.44 * cell * h + 0.22 * (1.0 - h);
    // Patches: some greyer and paler (weathered), some darker and warmer.
    c = mix(c * vec3(1.05, 0.95, 0.85) * 0.7, c * vec3(1.0, 1.02, 1.05) * 1.25, blotch);
    c *= 0.85 + 0.3 * fleck;
    roughness = mix(1.0, 0.8, h);
    ao *= mix(1.0 - 0.5 * relief, 1.0, h);

    // Moss: on the side away from the sun, on top of a limb and at the foot
    // of the trunk, in patches, filling the furrows first. Lichen: pale
    // crusts on the ridges.
    vec3 side = normalize(vec3(geomN.x, 0.0, geomN.z) + 1e-5);
    float grow = 0.55 * smoothstep(-0.2, 0.9, dot(side, BARK_MOSS_SIDE)) + 0.7 * smoothstep(0.25, 0.85, geomN.y) +
                 0.9 * exp(-max(hb, 0.0) / 0.6);
    float patchy = barkNoise(vec3(coords.xy, coords.z * 0.5) / (0.7 * patchSize) + 41.0) * 0.7 +
                   (seen > 0.0 ? mix(0.5, barkNoise(q * vec3(1.7, 1.7, 2.5) + 63.0), seen) : 0.5) * 0.3;
    float m = smoothstep(0.45, 0.8, moss * grow + 0.45 * patchy + 0.15 * (1.0 - h) - 0.2);
    vec3 mossC = vec3(0.075, 0.12, 0.025) * (0.75 + 0.5 * fleck);
    c = mix(c, mossC, m);
    // Lichen crusts, their mean (about a tenth of the bark) far off.
    float lichen = seen > 0.0 ? mix(0.1, smoothstep(0.72, 0.8, barkNoise(q * vec3(1.3, 1.3, 4.0) + 87.0)), seen) : 0.1;
    lichen *= h * (0.3 + 0.7 * moss);
    c = mix(c, vec3(0.34, 0.37, 0.3), 0.55 * lichen * (1.0 - m));
    N = normalize(mix(N, geomN, 0.6 * m));
    roughness = mix(roughness, 1.0, m);
    color = c;
}

#endif  // SCENE_BARK_VERTEX
