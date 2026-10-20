// Turf: the ground under a grass node (GrassNode, scene_grass.glsl), shaded
// as the sward the blades stand for and the forest floor under trees, so the
// ground continues the grass where the blades thin out and past their range,
// and reads as litter and soil where they do not grow. Everything comes from
// the grass node's field (density, dryness), its floor texture (how closed
// the canopy is overhead) and, off the field, the ground mesh's vertex
// colour carrying the same data (r density, g dryness, b floor), plus noise:
// nothing is baked per place.
//
//   - the sward's colour is the blades' mean (their lush and dry colours,
//     the field's dryness and the same clump noise), over the soil at their
//     roots near the eye, rising to the tops of the blades with distance as
//     the eye sees less between them;
//   - bare ground is soil; under trees it is the litter of fallen leaves (a
//     cell pattern of leaves of several browns), twigs, darker damp soil and
//     patches of moss;
//   - every pattern is band-limited: an octave finer than about two pixels
//     fades to its mean, so the turf has texture at any height without
//     shimmering, and keeps its mottling from far above.
//
// Needs scene_camera.glsl and scene_grass.glsl.

// A far gap among the blades: the sward seen deeper in, this much of its
// tops' light.
const float GRASS_TURF_DEPTH = 0.75;
// How much light comes through the far blades the turf stands for (half a
// far blade's: they stand over one another, the lower ones in shade).
const float GRASS_TURF_TRANSLUCENCY = 0.3;
// Past the blades, the sward's tops as the far fans show them: their
// leaves over the deeper ones between (GRASS_TURF_DEPTH where they part).
// (Wilds at the fans' edge, 85-110 m, golden and noon: 0.85 left the turf
// 1-4% darker than the fans looking across.)
const float GRASS_TURF_TOPS = 0.95;
// The herbs' blooms in the turf, against the share the far fans' dots
// show (scene_grass.glsl's grassFanBloom; the two meet at the blades' end).
const float GRASS_TURF_BLOOMS = 0.6;
// A blade's colour over its lush or dry colour on average: its hue's
// spread (scene_grass.glsl's `hue`, the blue cut a little more than it is
// lifted) and its paler, yellower sheath over its foot.
const vec3 GRASS_BLADE_MEAN_TINT = vec3(1.03, 1.025, 0.88);

float groundHash(ivec2 p, int k) {
    uint h = uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(k) * 0xcb1ab31fu;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// Value noise in [0, 1], an octave of period 1 / freq faded to its mean
// (0.5) as the period nears two pixels of footprint `fp` (m).
float groundOctave(vec2 p, float freq, float fp) {
    float seen = clamp(0.5 / (freq * fp) - 1.0, 0.0, 1.0);
    if (seen <= 0.0) return 0.5;
    return mix(0.5, sceneWindNoise(p * freq) * 0.5 + 0.5, seen);
}

// Several octaves from `freq` up, each half the amplitude.
float groundFbm(vec2 p, float freq, float fp, int octaves) {
    float s = 0.0, a = 0.5, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        s += a * groundOctave(p + float(i) * 17.31, freq, fp);
        n += a;
        a *= 0.5;
        freq *= 2.03;
    }
    // Stretched to about one octave's range: a sum crowds toward its mean.
    return clamp(0.5 + (s / n - 0.5) * 2.2, 0.0, 1.0);
}

// Fallen leaves: ovals of several sizes strewn `size` m apart, two to a
// cell, overlapping, the topmost seen. Returns the leaf's colour (or the
// soil's, in a gap), in `edge` how far inside its outline the point is (0 at
// the rim .. 1 at the middle) and in `shade` how lit it is (a leaf above
// casts a rim of shadow).
vec3 groundLeaves(vec2 xz, float size, out float edge, out float shade) {
    vec2 q = xz / size;
    ivec2 i = ivec2(floor(q));
    vec2 f = q - vec2(i);
    float top = -1.0, near = -1.0;
    float pick = 0.0, inside = 0.0, rib = 1.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 c = i + ivec2(x, y);
            for (int k = 0; k < 2; ++k) {
                int s = k * 16;
                vec2 o = vec2(groundHash(c, 1 + s), groundHash(c, 2 + s));
                float a = groundHash(c, 3 + s) * 6.2831853;
                float R = 0.45 + 0.5 * groundHash(c, 4 + s);
                vec2 v = (vec2(x, y) + o - f) / R;
                vec2 r = vec2(cos(a) * v.x + sin(a) * v.y, -sin(a) * v.x + cos(a) * v.y);
                // A leaf's outline: an oval, pointed at the tip.
                float d = length(r * vec2(1.0, 1.9)) + 0.25 * max(r.x, 0.0);
                float z = groundHash(c, 5 + s);
                if (d < 1.0 && z > top) {
                    top = z;
                    pick = groundHash(c, 6 + s);
                    inside = 1.0 - d;
                    rib = smoothstep(0.0, 0.05, abs(r.y));
                }
                if (d < 1.25) near = max(near, z);
            }
        }
    edge = top < 0.0 ? 0.0 : inside;
    shade = top < 0.0 ? 0.35 : (near > top ? 0.65 : 1.0);
    if (top < 0.0) return vec3(0.03, 0.022, 0.016);   // the soil between
    // Old leaves: tan, russet, brown, near black, a few still yellow.
    vec3 c = pick < 0.25 ? vec3(0.2, 0.135, 0.075)
           : pick < 0.45 ? vec3(0.16, 0.09, 0.05)
           : pick < 0.75 ? vec3(0.105, 0.08, 0.06)
           : pick < 0.92 ? vec3(0.06, 0.048, 0.036)
                         : vec3(0.26, 0.21, 0.09);
    return c * mix(0.8, 1.0, rib);
}

// Blades lying low (short live ones, fallen dead ones): in each `size` m cell
// one, a `keep` share of the cells, tapering to a point at each end. Returns
// the share of the pixel on a blade; `tone` the topmost one's own 0..1.
float groundBlades(vec2 xz, float size, float keep, float width, float fp, out float tone) {
    ivec2 i = ivec2(floor(xz / size));
    float on = 0.0;
    tone = 0.5;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 c = i + ivec2(x, y);
            if (groundHash(c, 21) > keep) continue;
            vec2 p = (vec2(c) + vec2(groundHash(c, 22), groundHash(c, 23))) * size;
            float a = groundHash(c, 24) * 3.14159265;
            vec2 d = vec2(cos(a), sin(a));
            float len = size * (0.6 + 0.9 * groundHash(c, 25));
            vec2 v = xz - p;
            float t = clamp(dot(v, d) / (0.5 * len), -1.0, 1.0);
            float dist = abs(dot(v, vec2(-d.y, d.x)));
            float w = size * width * (0.5 + 0.8 * groundHash(c, 26)) * sqrt(max(1.0 - t * t, 0.0));
            float cov = clamp((w + 0.5 * fp - dist) / fp, 0.0, 1.0) * min(w / max(fp, 1e-6) + 0.3, 1.0) *
                        step(abs(dot(v, d)), 0.5 * len);
            if (cov > on) {
                on = cov;
                tone = groundHash(c, 27);
            }
        }
    return on;
}

// Twigs: in each `size` m cell perhaps one, a thin dark stroke. 1 on a
// twig, 0 off one.
float groundTwigs(vec2 xz, float size, float fp) {
    ivec2 i = ivec2(floor(xz / size));
    float on = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 c = i + ivec2(x, y);
            if (groundHash(c, 11) > 0.45) continue;
            vec2 p = (vec2(c) + vec2(groundHash(c, 12), groundHash(c, 13))) * size;
            float a = groundHash(c, 14) * 3.14159265;
            vec2 d = vec2(cos(a), sin(a));
            float len = size * (0.4 + 0.8 * groundHash(c, 15));
            vec2 v = xz - p;
            float t = clamp(dot(v, d), -len * 0.5, len * 0.5);
            float dist = length(v - d * t);
            float w = size * 0.035 * (0.6 + 0.8 * groundHash(c, 16));
            on = max(on, 1.0 - smoothstep(w, w + fp, dist));
        }
    return on;
}

// The ground's data at `xz`: the grass node's field where it reaches (faded
// out over its last few metres, as the blades and herbs are), the mesh's own
// (`vdata`, its vertex colour: density, dryness, floor, herbs) beyond.
struct GroundData {
    float density, dry, under, herbs, inside;
};
GroundData groundData(vec2 xz, vec4 vdata) {
    vec4 data = clamp(vdata, 0.0, 1.0);
    float inside = grassFieldInside(xz);
    if (grassInField(xz)) {
        vec4 f = grassFieldAt(xz);
        vec2 fl = grassFloorHerbsAt(xz);
        vec4 fd = vec4(f.g, f.b, fl.x, grass.pushRate.z > 0.5 ? fl.y : 0.0);
        data = mix(data, fd, inside);
    }
    GroundData g;
    g.density = data.r;
    g.dry = data.g;
    g.under = data.b;
    g.herbs = data.a;
    g.inside = inside;
    return g;
}

// What the turf's surface layer keeps (docs/scene-world-architecture.md
// §5): every pattern of the turf that depends only on where it is, each
// band-limited to the footprint `fp` (a tile's texel as the tile is filled,
// the pixel's where a fragment works them out itself), 0..1 each:
//   a: the sward's clumps, its tall patches, its flowering patches' noise,
//      the big mottling;
//   b: the tussocks, the tufts, the grain, the soil's drift;
//   c: the forest floor's leaf heaps, damp hollows, moss, twigs;
//   d: the herbs' blooms here (herbBloomLocal): rgb their colour, a how
//      many more of them than their mean, m as m / (1 + m).
struct GroundBake {
    vec4 a, b, c, d;
};

// An octave of frequency `freq`'s share at footprint `fp`, as groundOctave
// fades it (to its mean as its period nears two footprints).
float groundSeen(float freq, float fp) { return clamp(0.5 / (freq * fp) - 1.0, 0.0, 1.0); }

GroundBake groundBake(vec2 xz, GroundData g, float fp) {
    GroundBake b;
    float k = grass.dry.w;
    float clump = (0.5 * sceneWindNoise(xz * k) + 0.25 * sceneWindNoise(xz * k * 2.03 + 17.3)) * 0.667;
    b.a.x = clamp(clump * groundSeen(k, fp) + 0.5, 0.0, 1.0);
    float tall = mix(0.5, sceneWindNoise(xz * 0.045 + 3.0) * 0.5 + 0.5, groundSeen(0.045, fp));
    b.a.y = smoothstep(0.35, 0.8, tall);
    b.a.z = mix(0.5, grassKindsNoise(xz), groundSeen(0.07, fp));
    b.a.w = groundFbm(xz, 0.035, fp, 3);
    b.b.x = groundFbm(xz + 31.0, 0.6, fp, 3);
    b.b.y = groundFbm(xz + 57.0, 3.1, fp, 2);
    b.b.z = groundFbm(xz * vec2(1.0, 1.6) + 77.0, 9.0, fp, 2);
    b.b.w = groundFbm(xz + 5.0, 0.25, fp, 3);
    b.c.x = groundFbm(xz + 23.0, 2.2, fp, 3);
    b.c.y = groundFbm(xz + 51.0, 0.12, fp, 3);
    b.c.z = groundFbm(xz + 91.0, 0.3, fp, 3);
    b.c.w = groundTwigs(xz, 0.35, fp) * clamp(0.35 / fp * 0.1 - 0.3, 0.0, 1.0);
    b.d = vec4(0.0);
    if (grass.herbInfo.x > 0.5 && g.herbs > 0.0) {
        vec3 bloom, pick;
        float more = herbBloomLocal(xz, g.under, -1.0, fp, bloom, pick);
        b.d = vec4(bloom, more / (1.0 + more));
    }
    return b;
}

#ifndef GROUND_LAYER_FILL
#include "scene_ground_layers.glsl"

// The ground at a fragment: `rel` its camera-relative position, `vdata` the
// vertex colour (density, dryness, floor) for where the field does not
// reach. Sets the colour, the roughness and the occlusion.
void groundSurface(vec3 rel, vec4 vdata, inout vec3 color, inout vec3 N, inout float roughness, inout float ao,
                   out vec4 thin, out vec4 swardLight) {
    vec3 eye = camera.eyeWorld.xyz;
    vec2 xz = rel.xz + eye.xz;
    float fp = max(length(fwidth(rel)), 1e-4);

    GroundData g = groundData(xz, vdata);
    float density = g.density, dry = g.dry, under = g.under, herbs = g.herbs, inside = g.inside;
    // Its patterns from the surface layer, or worked out here where no
    // tile holds them yet.
    GroundBake b;
    if (!groundLayerSample(xz, fp, b)) b = groundBake(xz, g, fp);

    // The sward: the blades' mean colour (scene_grass.glsl's), its clumps
    // and its large patches of taller, lusher grass.
    float clump = b.a.x;
    float lush = 1.0 - dry;
    vec3 green = grass.lush.rgb;
    vec3 dryc = grass.dry.rgb;
    // (Their shade along them as the eye sees it on average: the far
    // blades' grassMeanShade.)
    // (And their hue's spread and paler, yellower sheaths, on average a
    // little warmer: scene_grass.glsl's blade colour.)
    vec3 sward = mix(dryc, green, clamp(lush * 1.3 - 0.15 + clump * 0.2, 0.0, 1.0)) * grassMeanShade() *
                 GRASS_BLADE_MEAN_TINT;
    // With its dead leaves and its flowering heads, as the blades have them
    // there (scene_grass.glsl's grassKinds).
    float tallN = b.a.y;
    vec2 kinds = grassKindsOf(b.a.z, lush, tallN);
    sward = mix(sward, grassDeadColor(0.5), kinds.y * 0.45);
    // (Seen across, their haze: scene_grass_surface.glsl's far heads.)
    float ripe = clamp(1.1 - lush, 0.0, 1.0);
    float graze = 1.0 - smoothstep(0.05, 0.4, -rel.y / max(length(rel.xz), 1e-3));
    sward = mix(sward, mix(grassHeadColor(ripe), grassHeadFarColor(ripe), graze), min(kinds.x * 0.35, 0.3));
    // How much of the ground the blades cover, seen from above.
    float cover = clamp(density * (0.35 + 0.9 * clump) * 1.4, 0.0, 1.0);
    // Between the blades the ground shows: their dark roots and the soil
    // near the eye; further off, where a gap is a few blades' depth of the
    // sward rather than the ground under it, the blades seen deeper in, in
    // their shade (a gap among far blades is more grass, not a paler
    // ground). Where the blades end only their tops are seen.
    float d = length(vec2(length(rel.xz), grass.lod.w));   // as the rings measure it
    // (Off the field there are none: there it is their tops at any distance.)
    // The tops show as the far ring's blades dither out over its band
    // (scene_grass.glsl's fout), not before: under the blades still drawn a
    // gap is the sward's depth.
    float topSeen = 1.0 - (1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, d)) * inside;
    vec3 soil = vec3(0.065, 0.048, 0.03);
    float deep = smoothstep(10.0, 40.0, d);
    vec3 roots = mix(mix(soil, sward * grass.look.x, 0.6), sward * GRASS_TURF_DEPTH, deep);
    // Close by, what lies between the blades: thatch (dead blades, pale
    // straws lying every way), crumbs of soil, and the green of the bases.
    float nearSeen = clamp(0.012 / fp - 0.6, 0.0, 1.0) * (1.0 - topSeen);
    if (nearSeen > 0.0) {
        float crumb = groundFbm(xz + 13.0, 45.0, fp, 2);
        float base = groundFbm(xz + 41.0, 14.0, fp, 2);
        // The foot of the sward: dark, the bases' green in shadow, crumbs
        // of soil showing in its thin places.
        float fine = groundFbm(xz + 7.0, 110.0, fp, 1);
        float bare = smoothstep(0.66, 0.86, 0.6 * fine + 0.4 * crumb + 0.35 * (1.0 - density) - 0.2 * base) * 0.8;
        vec3 nearRoots = mix(sward * grass.look.x * (0.25 + 0.15 * crumb), soil * (0.4 + 0.4 * fine), bare);
        // Short live blades lying every way over it, each its own tone (some
        // turned to the light), and fewer fallen dead ones, thin and pale to
        // dun, tapering to their points.
        float tA, tB, tS;
        vec2 xr = vec2(xz.x * 0.5 - xz.y * 0.866, xz.x * 0.866 + xz.y * 0.5);
        float lowA = groundBlades(xr + 5.3, 0.05, 0.85, 0.07, fp, tA);
        float lowB = groundBlades(vec2(xz.y, -xz.x) + 2.9, 0.04, 0.75, 0.07, fp, tB);
        float straw = groundBlades(xz + vec2(3.7, 1.3), 0.08, 0.3 + 0.4 * dry, 0.03, fp, tS);
        vec3 live = sward * grass.look.x;
        nearRoots = mix(nearRoots, live * (0.6 + 0.7 * tA), lowA * 0.9);
        nearRoots = mix(nearRoots, mix(live, grass.dry.rgb * 0.7, 0.3 * tB) * (0.5 + 0.6 * tB), lowB * 0.85);
        vec3 strawC = mix(vec3(0.11, 0.09, 0.055), grass.dry.rgb * 0.85, tS) * (0.75 + 0.4 * crumb);
        nearRoots = mix(nearRoots, strawC, straw * 0.8);
        roots = mix(roots, nearRoots, nearSeen);
    }
    // Seen from above, the tops there part over their gaps as the far
    // fans' do: the gaps (the sward's depth) are 0.9 of the sine of the
    // eye's line down of what is seen there, none seen across.
    float steepT = -rel.y / max(length(rel), 1e-3);
    vec3 turf = mix(roots, sward * GRASS_TURF_TOPS, topSeen * (1.0 - 0.9 * steepT));

    // Mottling at every scale: big patches (a few metres to tens), tussocks
    // (a metre or so), the grain of blades (centimetres).
    float big = b.a.w;
    float tuss = b.b.x;
    float tuft = b.b.y;
    float grain = b.b.z;
    // Seen between drawn blades (past the near ground) the turf is their
    // shaded depth: the blades carry the sward's own patches, so it is
    // mottled less and not bleached (pale patches under far blades read as
    // a different, sun-dried ground).
    float mot = mix(1.0, 0.35, (1.0 - topSeen) * smoothstep(8.0, 25.0, d));
    turf *= mix(1.0, 0.72 + 0.56 * big, mot);
    turf = mix(turf, turf * vec3(1.15, 1.08, 0.75), smoothstep(0.55, 0.8, big) * 0.35 * mot * mot);   // sun-bleached
    turf *= mix(1.0, 0.6 + 0.8 * tuss, mot);
    turf = mix(turf, turf * vec3(1.1, 1.05, 0.8), smoothstep(0.6, 0.8, tuss) * 0.5 * mot * mot);    // drier tussock tops
    turf *= mix(1.0, 0.75 + 0.5 * tuft, mot);
    turf *= 0.85 + 0.3 * grain;

    // The forest floor: litter where the canopy closes, soil where nothing
    // grows in the open (there the litter is not worked out at all: it was
    // two fifths of an open ground's turf, of the wilds' land round its
    // world all of it).
    float drift = b.b.w;
    vec3 bare = soil * (0.75 + 0.5 * drift) * (0.85 + 0.3 * grain);
    vec3 floorC = bare;
    float litterAo = 1.0;
    if (under > 0.0) {
        float lf;
        float lshade;
        float leafSize = 0.055;
        float leafSeen = clamp(leafSize / fp * 0.4 - 0.5, 0.0, 1.0);
        vec3 litter = vec3(0.095, 0.07, 0.048);   // the leaves' mean
        if (leafSeen > 0.0 && under > 0.02) {
            vec3 leaf = groundLeaves(xz, leafSize, lf, lshade);
            vec3 l = leaf * lshade * mix(0.7, 1.0, smoothstep(0.0, 0.15, lf));
            litter = mix(litter, l, leafSeen);
            litterAo = mix(1.0, mix(0.7, 1.0, lshade), leafSeen);
        }
        // Leaf drifts (some fresher and russet, some old and grey), damp dark
        // hollows and moss, at scales that hold from a few metres to far above.
        float heap = b.c.x;
        float damp = b.c.y;
        float mossy = b.c.z;
        litter *= 0.65 + 0.7 * drift;
        litter *= 0.7 + 0.6 * heap;
        litter = mix(litter * vec3(0.85, 0.9, 1.0), litter * vec3(1.25, 1.0, 0.75), smoothstep(0.3, 0.7, heap));
        litter = mix(litter, vec3(0.03, 0.023, 0.017), smoothstep(0.55, 0.75, damp) * 0.75);
        litter = mix(litter, vec3(0.045, 0.075, 0.018) * (0.7 + 0.6 * tuss), smoothstep(0.58, 0.72, mossy) * 0.85);
        // (A twig is a centimetre or so across: finer than the finest
        // tile's texel, it is drawn here.)
        float twig = fp < groundLayerFinest() ? groundTwigs(xz, 0.35, fp) * clamp(0.35 / fp * 0.1 - 0.3, 0.0, 1.0)
                                               : b.c.w;
        litter = mix(litter, vec3(0.045, 0.032, 0.022), twig * 0.85);
        floorC = mix(bare, litter, smoothstep(0.0, 0.3, under));
    }

    // Far off, a thin clump's ground is hidden by the blades around it (they
    // lean over it, and the eye sees across): only thin grass leaves soil.
    cover = mix(cover, clamp(density * 1.4, 0.0, 1.0), 0.7 * deep);
    color = mix(floorC, turf, cover);
    // Where it stands for the blades' tops it is lit as they are from afar
    // (scene_grass.glsl's grassSwardUp, SceneSurface::sward), not as level
    // ground.
    float asBlades = cover * max(topSeen, deep);
    swardLight = vec4(0.0);
    if (asBlades > 0.0) {
        // Tipped by the sward's lay and its gusts as the far blades are,
        // and a gust lifts the sheen of what it bends (scene_grass.glsl).
        float gu = sceneWindGustAt(xz);
        vec3 up = grassSwardUp(N, grassCombLean(xz, gu, fp));
        N = normalize(mix(N, grassCanopyNormal(up, normalize(-rel)), asBlades));
        // Past the blades the sun on it is all the sward's, as on the far
        // blades (mesh.frag's grass, its weight 1 from 12 m): the share left
        // to the canopy normal, which faces the eye, lit it by the eye's
        // line, not the sun's.
        swardLight = vec4(up, mix(asBlades, 1.0, topSeen));
        color *= 1.0 + grass.look.y * (gu * grassGustStir() * 0.7 + grassGustFar(gu)) * asBlades;
    }
    ao *= mix(litterAo, 1.0, cover) * mix(1.0, 0.8, asBlades);   // (a far blade's ao)
    roughness = mix(1.0, 0.9, asBlades);
    // Past the blades the low sun glows through the tops it stands for as
    // through the far blades (their faces turned to the eye; mesh.frag's
    // translucency, as theirs); a gap among drawn blades is in their shade.
    vec3 faceEye = normalize(vec3(-rel.x, 0.3 * length(rel.xz), -rel.z));
    thin = vec4(faceEye, GRASS_TURF_TRANSLUCENCY * cover * topSeen);

    // The herbs past their plants (scene_herb_mix.glsl): their leaves' and
    // blooms' mean over the share of the ground they cover as the eye sees
    // it (more of them seen across than from above), so the turf carries a
    // meadow's flowers and a forest floor's ferns to the horizon.
    // (Their mean over the colonies, from the node's table by the canopy:
    // weighing every species here per pixel cost the turf's whole pass a
    // tenth of a millisecond, even where no herbs grow.)
    if (grass.herbInfo.x > 0.5 && herbs > 0.0) {
        float tanView = -rel.y / max(length(rel.xz), 1e-3);
        float stand = 1.0 - herbPlantsDrawn(d, grass.lod.w / max(length(rel.xz), 1e-3)) * inside;
        if (stand > 0.0) {
            HerbMix hm = herbMixMean(under, herbs, tanView);
            // The blooms patch by patch (herbBloomLocal): a colony's colour
            // where it grows, more of them inside it, none between. As much
            // of them as of the far blades (scene_grass.glsl), between and
            // past them; their leaves mostly down among the grass, as the
            // plants' are (GRASS_HERB_LEAF_SEEN).
            vec3 bloom = b.d.rgb;
            float more = b.d.a / max(1.0 - b.d.a, 1e-3);
            float share = min(hm.bloomShare * more, 0.95);
            float leaves = hm.cover * (1.0 - hm.bloomShare) * GRASS_HERB_LEAF_SEEN;
            float blooms = min(hm.cover * share, 1.0) * GRASS_HERB_BLOOM_SEEN * GRASS_TURF_BLOOMS;
            // Their mass mottled as the sward's is (tussocks, patches),
            // their tops lit over their shaded depths.
            float mott = (0.75 + 0.5 * tuft) * (0.8 + 0.4 * tuss);
            color = mix(color, hm.leaf * mott, leaves * stand);
            color = mix(color, bloom * mott, blooms * stand);
        }
    }
}
#endif  // GROUND_LAYER_FILL
