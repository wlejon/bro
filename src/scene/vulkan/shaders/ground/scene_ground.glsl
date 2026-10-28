// Turf: the ground under a grass node (GrassNode, scene_grass.glsl), shaded
// as the sward the blades stand for and the forest floor under trees, so the
// ground continues the grass where the blades thin out and past their range,
// and reads as litter and soil where they do not grow. Everything comes from
// the grass node's field (density, dryness), its floor texture (how closed
// the canopy is overhead) and, off the field, the ground mesh's vertex
// colour carrying the same data (r density, g dryness, b floor), plus noise:
// nothing is baked per place.
//
//   - the sward is the world's sward module's (swardTurf): its tops' colour
//     and what shows between its blades, rising to the tops with distance
//     as the eye sees less between them, lit as its blades (swardLay);
//   - bare ground is soil; under trees it is the litter of fallen leaves (a
//     cell pattern of leaves of several browns), twigs, darker damp soil and
//     patches of moss;
//   - every pattern is band-limited (scene_ground_noise.glsl), so the turf
//     has texture at any height without shimmering, and keeps its mottling
//     from far above.
//
// Needs scene_camera.glsl and scene_grass.glsl.

// How much light comes through the far blades the turf stands for (half a
// far blade's: they stand over one another, the lower ones in shade).
const float GRASS_TURF_TRANSLUCENCY = 0.3;
// The share of the sward's light through it that comes in its blades' lobe
// about the sun (scene_lighting.glsl's swardGlow), the rest even: the far
// fans' own pixels glow in the lobe, the turf seen between them evenly,
// and the turf past them stands for both. (Wilds, noon, looking across at
// 85-110 m: all of it in the lobe left the turf 3.5% darker than the fans.)
const float GRASS_TURF_GLOW_SHARE = 0.8;
// The herbs' blooms in the turf, against the share the far fans' dots
// show (scene_grass.glsl's grassSpeck; the two meet at the blades' end).
const float GRASS_TURF_BLOOMS = 0.6;

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

// The twigs at footprint `fp`, faded out as a twig nears a pixel's width:
// none from 0.117 m, and there their nine cells are not looked at.
float groundTwigsSeen(vec2 xz, float fp) {
    float seen = clamp(0.35 / fp * 0.1 - 0.3, 0.0, 1.0);
    return seen > 0.0 ? groundTwigs(xz, 0.35, fp) * seen : 0.0;
}

// The pixel footprints (m) over which turf on a land hands over to the
// land's own surface (mesh.frag): its mottling's largest patches are tens of
// metres.
const float TURF_FAR_FROM = 6.0;
const float TURF_FAR_TO = 24.0;

// The ground's data at `xz`: the land's cover where it reaches (faded out
// over its last few metres, as the blades and herbs are), the mesh's own
// (`vdata`, its vertex colour: density, dryness, floor, herbs) beyond.
struct GroundData {
    float density, dry, under, herbs, inside;
    vec3 surface;   // the land's surface's albedo (on the land: where `inside`)
};
GroundData groundData(vec2 xz, vec4 vdata) {
    vec4 data = clamp(vdata, 0.0, 1.0);
    float inside = grassFieldInside(xz);
    vec3 albedo = vec3(0.0);
    if (grassInField(xz)) {
        vec4 f;
        vec2 fl;
        vec3 n;
        grassGround(xz, f, fl, n, albedo);
        data = mix(data, vec4(f.g, f.b, fl.x, fl.y), inside);
    }
    GroundData g;
    g.density = data.r;
    g.dry = data.g;
    g.under = data.b;
    g.herbs = data.a;
    g.inside = inside;
    g.surface = albedo;
    return g;
}

// What the turf's surface layer keeps (docs/scene-world-architecture.md
// §5): every pattern of the turf that depends only on where it is, each
// band-limited to the footprint `fp` (a tile's texel as the tile is filled,
// the pixel's where a fragment works them out itself), 0..1 each:
//   a: xyz the sward module's (swardBake), w the big mottling;
//   b: the tussocks, the tufts, the grain, the soil's drift;
//   c: the forest floor's leaf heaps, damp hollows, moss, twigs;
//   d: the herbs' blooms here (herbFarLocal): rgb their colour, a how
//      many more of them than their mean, m as m / (1 + m).
struct GroundBake {
    vec4 a, b, c, d;
};

GroundBake groundBake(vec2 xz, GroundData g, float fp) {
    GroundBake b;
    b.a.xyz = swardBake(xz, fp);
    b.a.w = groundFbm(xz, 0.035, fp, 3);
    b.b.x = groundFbm(xz + 31.0, 0.6, fp, 3);
    b.b.y = groundFbm(xz + 57.0, 3.1, fp, 2);
    b.b.z = groundFbm(xz * vec2(1.0, 1.6) + 77.0, 9.0, fp, 2);
    b.b.w = groundFbm(xz + 5.0, 0.25, fp, 3);
    // (The forest floor's only where there is one, and its twigs only where
    // a pixel can show them: worked out everywhere they were a fifth of the
    // turf's cost seen from a kilometre up.)
    b.c = vec4(0.5, 0.5, 0.5, 0.0);
    if (g.under > 0.0) {
        b.c.x = groundFbm(xz + 23.0, 2.2, fp, 3);
        b.c.y = groundFbm(xz + 51.0, 0.12, fp, 3);
        b.c.z = groundFbm(xz + 91.0, 0.3, fp, 3);
        b.c.w = groundTwigsSeen(xz, fp);
    }
    b.d = vec4(0.0);
    if (grass.herbInfo.x > 0.5 && g.herbs > 0.0) {
        vec3 bloom, pick;
        float more = herbFarLocal(xz, g.under, -1.0, fp, bloom, pick);
        b.d = vec4(bloom, more / (1.0 + more));
    }
    return b;
}

#ifndef GROUND_LAYER_FILL
#include "ground/scene_ground_layers.glsl"

// The sine of the eye's line down past which the turf's sward is lit as
// level ground, all of it straight down: as the land past the turf is lit
// (mesh.frag), its albedo the meadow's as seen from above. Lit as the
// blades' shaded depth (SceneSurface::sward) all the way down, a meadow
// seen straight down was a stop darker than the land it hands over to
// (TURF_FAR_FROM): the same ground brightening as the eye rose over it.
// Along low lines the two agree.
const float TURF_LEVEL_FROM = 0.5;

// The turf's far look at `xz` (groundShade's, its data `g`): what a pixel
// past all its patterns sees of the ground there, the blades' tops over
// their depth, bare soil or the land's surface where nothing grows, the
// litter where a canopy closes, every pattern at its mean (the mottling's
// factors 1 there, its bleaching none), and (groundShade adds them) the
// herbs at their mean, as though none of their plants were drawn. The
// land's surface past the turf is this ground as such a pixel sees it, so
// mesh.frag scales the turf by the land's albedo over this: the turf adds
// its patterns to the land's colour, with no mean of their own, and hands
// over to the land with nothing to change.
vec3 groundFarMean(vec2 xz, vec3 rel, GroundData g, vec3 soil, float steepT) {
    SwardTurf st = swardTurf(SwardTurfIn(xz, rel, 1e6, g.density, g.dry, swardBake(xz, 1e6), 1e6, 1.0, 0.0, soil));
    vec3 floorC = mix(soil, g.surface, (1.0 - smoothstep(0.0, 0.15, g.density)) * g.inside);
    if (g.under > 0.0) {
        vec3 litter = vec3(0.095, 0.07, 0.048);
        litter = mix(litter * vec3(0.85, 0.9, 1.0), litter * vec3(1.25, 1.0, 0.75), 0.5);
        floorC = mix(floorC, litter, smoothstep(0.0, 0.3, g.under));
    }
    return mix(floorC, mix(st.roots, st.tops, 1.0 - 0.9 * steepT), st.cover);
}

// The ground at the cover's frame's point `xz` (scene_grass.glsl; a round
// body's cell's, coverUse), the fragment camera-relative at `rel` with the
// footprint `fp`: its patterns `b` from the surface layer where `baked`,
// else worked out here. `farMean`: the turf's far look here, every pattern
// at its mean (groundFarMean), which the colour refines.
void groundShade(vec2 xz, vec3 rel, float fp, vec4 vdata, bool baked, GroundBake b, inout vec3 color,
                 inout vec3 N, inout float roughness, inout float ao, out vec4 thin, out vec4 swardLight,
                 out vec3 farMean) {
    GroundData g = groundData(xz, vdata);
    float density = g.density, dry = g.dry, under = g.under, herbs = g.herbs, inside = g.inside;
    if (!baked) b = groundBake(xz, g, fp);

    // The sward (the module's swardTurf): its tops, and between its blades
    // their roots and the soil near the eye; further off, where a gap is a
    // few blades' depth of the sward rather than the ground under it, more
    // sward deeper in. Where the blades end only their tops are seen.
    float d = length(vec2(length(rel.xz), grass.lod.w));   // as the rings measure it
    // (Off the field there are none: there it is their tops at any distance.)
    // The tops show as the far ring's blades dither out over its band
    // (scene_grass.glsl's fout), not before: under the blades still drawn a
    // gap is the sward's depth.
    float topSeen = 1.0 - (1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, d)) * inside;
    vec3 soil = vec3(0.065, 0.048, 0.03);
    float deep = smoothstep(10.0, 40.0, d);
    // Close by, the ground between the blades has detail of its own.
    float nearSeen = clamp(0.012 / fp - 0.6, 0.0, 1.0) * (1.0 - topSeen);
    SwardTurf st = swardTurf(SwardTurfIn(xz, rel, fp, density, dry, b.a.xyz, d, deep, nearSeen, soil));
    float cover = st.cover;
    // Seen from above, the tops there part over their gaps as the far
    // blades' do: the gaps (the sward's depth) are 0.9 of the sine of the
    // eye's line down of what is seen there, none seen across.
    float steepT = -rel.y / max(length(rel), 1e-3);
    vec3 turf = mix(st.roots, st.tops, topSeen * (1.0 - 0.9 * steepT));

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
    // Where the land grows nothing (its surface's cover none) its surface
    // shows as the surface has it (its snow, rock, sand); soil is what lies
    // between blades.
    bare = mix(bare, g.surface, (1.0 - smoothstep(0.0, 0.15, density)) * inside);
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
        float twig = fp < groundLayerFinest() ? groundTwigsSeen(xz, fp) : b.c.w;
        litter = mix(litter, vec3(0.045, 0.032, 0.022), twig * 0.85);
        floorC = mix(bare, litter, smoothstep(0.0, 0.3, under));
    }

    color = mix(floorC, turf, cover);    // Where it stands for the blades' tops it is lit as they are from afar
    // (the module's swardLay, SceneSurface::sward), not as level ground.
    float asBlades = cover * max(topSeen, deep);
    swardLight = vec4(0.0);
    if (asBlades > 0.0) {
        // Tipped by the sward's lay and its gusts as the far blades are,
        // and a gust lifts the sheen of what it bends.
        vec4 lay = swardLay(xz, coverUnturn(N));
        lay.xyz = coverTurn(lay.xyz);
        // (Steeply down, level ground: TURF_LEVEL_FROM.)
        float level = smoothstep(TURF_LEVEL_FROM, 1.0, steepT);
        N = normalize(mix(N, grassCanopyNormal(lay.xyz, normalize(-rel)), asBlades * (1.0 - level)));
        // Past the blades the sun on it is all the sward's, as on the far
        // blades (their weight 1 from 12 m): the share left to the canopy
        // normal, which faces the eye, lit it by the eye's line, not the
        // sun's. Where the sward thins out to bare ground the share goes with
        // it: all of it on any cover drew a hard line where the cover ends
        // (a planet's grass line along its hills, seen from their height).
        swardLight = vec4(lay.xyz, mix(asBlades, 1.0, topSeen) * smoothstep(0.0, 0.25, cover) * (1.0 - level));
        color *= 1.0 + lay.w * asBlades;
    }
    ao *= mix(litterAo, 1.0, cover) * mix(1.0, 0.8, asBlades);   // (a far blade's ao)
    roughness = mix(1.0, 0.9, asBlades);
    // Past the blades the low sun glows through the tops it stands for as
    // through the far blades (their faces turned to the eye; mesh.frag's
    // translucency, as theirs); a gap among drawn blades is in their shade.
    vec3 faceEye = normalize(vec3(-rel.x, 0.3 * length(rel.xz), -rel.z));
    thin = vec4(faceEye * sqrt(GRASS_TURF_GLOW_SHARE), GRASS_TURF_TRANSLUCENCY * cover * topSeen);

    // The herbs past their plants (the module's herbFarMean): their leaves'
    // and blooms' mean over the share of the ground they cover as the eye
    // sees it (more of them seen across than from above), so the turf
    // carries a meadow's flowers and a forest floor's ferns to the horizon.
    // (A module's mean is cheap, a table by the canopy: weighing every
    // species here per pixel cost the turf's whole pass a tenth of a
    // millisecond, even where no herbs grow.)
    farMean = groundFarMean(xz, rel, g, soil, steepT);
    if (grass.herbInfo.x > 0.5 && herbs > 0.0) {
        float tanView = -rel.y / max(length(rel.xz), 1e-3);
        float stand = 1.0 - herbPlantsDrawn(d, grass.lod.w / max(length(rel.xz), 1e-3)) * inside;
        HerbMix hm = herbFarMean(under, herbs, tanView);
        {
            float leaves = hm.cover * (1.0 - hm.bloomShare) * GRASS_HERB_LEAF_SEEN;
            float blooms = min(hm.cover * min(hm.bloomShare, 0.95), 1.0) * GRASS_HERB_BLOOM_SEEN * GRASS_TURF_BLOOMS;
            farMean = mix(mix(farMean, hm.leaf, leaves), hm.bloom, blooms);
        }
        if (stand > 0.0) {
            // The blooms patch by patch (herbFarLocal): a colony's colour
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

// The ground at a fragment: `rel` its camera-relative position, `fp` its
// footprint (the length of fwidth(rel), taken by the caller in uniform
// control flow: no derivative is taken here, so a caller may skip it),
// `vdata` the vertex colour (density, dryness, floor) for where the field
// does not reach. Sets the colour, the roughness and the occlusion. On a round body
// in the frame of the lattice cell holding it, across a cell's edge the
// cells either side blended (scene_grass.glsl's coverTurfCells). `lc` is the
// chart place of its map point, landChartOf(landMapOf(rel + landEye3())),
// the caller's (it reads the land there too). `farMean` the colour's far
// look (groundFarMean).
void groundSurface(vec3 rel, float fp, vec4 vdata, LandChart lc, inout vec3 color, inout vec3 N, inout float roughness,
                   inout float ao, out vec4 thin, out vec4 swardLight, out vec3 farMean) {
    GroundBake b;
    if (!landRound()) {
        vec2 xz = rel.xz + camera.eyeWorld.xz;
        bool baked = groundLayerSample(groundPosOf(xz), fp, b);
        groundShade(xz, rel, fp, vdata, baked, b, color, N, roughness, ao, thin, swardLight, farMean);
        return;
    }
    vec3 p = rel + landEye3();
    bool baked = groundLayerSample(groundChartPosOf(lc), fp, b);
    CoverCell cells[4];
    float w[4];
    int n = coverTurfCells(p, lc, cells, w);
    vec3 c0 = color, n0 = N, cSum = vec3(0.0), nSum = vec3(0.0);
    float r0 = roughness, a0 = ao, rSum = 0.0, aSum = 0.0;
    vec4 tSum = vec4(0.0), sSum = vec4(0.0);
    vec3 fSum = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        vec4 rows[4];
        coverCellFrame(cells[i], rows);
        coverUse(rows);
        vec3 c = c0, nn = n0, f;
        float r = r0, a = a0;
        vec4 t, s;
        groundShade(coverFlatOf(rows, p).xz, rel, fp, vdata, baked, b, c, nn, r, a, t, s, f);
        cSum += w[i] * c;
        fSum += w[i] * f;
        nSum += w[i] * nn;
        rSum += w[i] * r;
        aSum += w[i] * a;
        tSum += w[i] * t;
        sSum += w[i] * s;
    }
    coverDone();
    color = cSum;
    N = normalize(nSum);
    roughness = rSum;
    ao = aSum;
    thin = tSum;
    swardLight = sSum;
    farMean = fSum;
}

// The ground's light where the land stands for the turf (mesh.frag past it,
// landFar), at the frame's point `xz`: the colour (the land's
// albedo) lit as groundShade lights the blades' tops seen past all their
// patterns, every pattern at its mean and the tops all that is seen: their
// lay's normal and sheen, the sward's light, a far blade's occlusion and
// roughness, the low sun through them. Lit as level ground instead, the
// land past the turf was a fifth to a third of a stop brighter than the
// turf it hands over to, seen from 1.5 km up 4 to 15 km off (capture.js
// compare's looks), and its dry meadows stood out pale where the turf's
// were green.
void groundFarShade(vec2 xz, vec3 rel, vec4 vdata, inout vec3 color, inout vec3 N, inout float roughness,
                    inout float ao, out vec4 thin, out vec4 swardLight) {
    GroundData g = groundData(xz, vdata);
    vec3 soil = vec3(0.065, 0.048, 0.03);
    float cover = swardTurf(SwardTurfIn(xz, rel, 1e6, g.density, g.dry, swardBake(xz, 1e6), 1e6, 1.0, 0.0, soil)).cover;
    swardLight = vec4(0.0);
    if (cover > 0.0) {
        // (The sward's lay at its mean, standing up from the ground: its
        // gusts are far under a pixel here, and read through swardLay they
        // changed no 32 px block of the planet's low or region shot.)
        vec3 lay = N;
        float steepT = -rel.y / max(length(rel), 1e-3);
        float level = smoothstep(TURF_LEVEL_FROM, 1.0, steepT);
        N = normalize(mix(N, grassCanopyNormal(lay, normalize(-rel)), cover * (1.0 - level)));
        swardLight = vec4(lay, smoothstep(0.0, 0.25, cover) * (1.0 - level));
    }
    ao *= mix(1.0, 0.8, cover);
    roughness = mix(1.0, 0.9, cover);
    vec3 faceEye = normalize(vec3(-rel.x, 0.3 * length(rel.xz), -rel.z));
    thin = vec4(faceEye * sqrt(GRASS_TURF_GLOW_SHARE), GRASS_TURF_TRANSLUCENCY * cover);
}

// groundFarShade at a fragment. Not in a round body's lattice cell's frame
// as groundSurface takes groundShade: nothing it reads there has a pattern
// a cell's frame places (the grass node's field never reaches the land
// past the turf, the sward is read at its mean), and finding the cells
// and their blend was three fifths of its cost (0.31 ms of the planet's
// land from a region's height) for no 32 px block changed.
void groundSurfaceFar(vec3 rel, vec4 vdata, inout vec3 color, inout vec3 N, inout float roughness, inout float ao,
                      out vec4 thin, out vec4 swardLight) {
    groundFarShade(rel.xz + camera.eyeWorld.xz, rel, vdata, color, N, roughness, ao, thin, swardLight);
}
#endif  // GROUND_LAYER_FILL
