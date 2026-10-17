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

// The ground at a fragment: `rel` its camera-relative position, `vdata` the
// vertex colour (density, dryness, floor) for where the field does not
// reach. Sets the colour, the roughness and the occlusion.
void groundSurface(vec3 rel, vec4 vdata, inout vec3 color, inout float roughness, inout float ao) {
    vec3 eye = camera.eyeWorld.xyz;
    vec2 xz = rel.xz + eye.xz;
    float fp = max(length(fwidth(rel)), 1e-4);

    // The ground's data: the field where it reaches (faded out over its last
    // few metres), the mesh's own beyond.
    vec3 data = clamp(vdata.rgb, 0.0, 1.0);
    if (grassInField(xz)) {
        vec2 q = (xz - grass.field.xy) / grass.field.z;
        vec2 toEdge = min(q, grass.fieldSize.xy - 1.0 - q) * grass.field.z;
        float inside = smoothstep(0.0, 12.0, min(toEdge.x, toEdge.y));
        vec4 f = grassFieldAt(xz);
        vec3 fd = vec3(f.g, f.b, grassFloorAt(xz));
        data = mix(data, fd, inside);
    }
    float density = data.r, dry = data.g, under = data.b;

    // The sward: the blades' mean colour (scene_grass.glsl's), its clumps
    // and its large patches of taller, lusher grass.
    float k = grass.dry.w;
    float clump = clamp((0.5 * sceneWindNoise(xz * k) + 0.25 * sceneWindNoise(xz * k * 2.03 + 17.3)) * 0.667 + 0.5,
                        0.0, 1.0);
    float lush = 1.0 - dry;
    vec3 green = grass.lush.rgb;
    vec3 dryc = grass.dry.rgb;
    vec3 sward = mix(dryc, green, clamp(lush * 1.3 - 0.15 + clump * 0.2, 0.0, 1.0)) * 0.95;
    // How much of the ground the blades cover, seen from above.
    float cover = clamp(density * (0.35 + 0.9 * clump) * 1.4, 0.0, 1.0);
    // Near the eye the ground shows between the blades: their dark roots and
    // the soil. Toward the blades' range only their tops are seen.
    float d = length(vec2(length(rel.xz), grass.lod.w));   // as the rings measure it
    float topSeen = smoothstep(grass.ring.y * 0.15, grass.ring.y * 0.85, d);
    vec3 soil = vec3(0.065, 0.048, 0.03);
    vec3 roots = mix(soil, sward * grass.look.x, 0.6);
    vec3 turf = mix(roots, sward, topSeen);

    // Mottling at every scale: big patches (a few metres to tens), tussocks
    // (a metre or so), the grain of blades (centimetres).
    float big = groundFbm(xz, 0.035, fp, 3);
    float tuss = groundFbm(xz + 31.0, 0.6, fp, 3);
    float tuft = groundFbm(xz + 57.0, 3.1, fp, 2);
    float grain = groundFbm(xz * vec2(1.0, 1.6) + 77.0, 9.0, fp, 2);
    turf *= 0.72 + 0.56 * big;
    turf = mix(turf, turf * vec3(1.15, 1.08, 0.75), smoothstep(0.55, 0.8, big) * 0.6);   // sun-bleached
    turf *= 0.6 + 0.8 * tuss;
    turf = mix(turf, turf * vec3(1.1, 1.05, 0.8), smoothstep(0.6, 0.8, tuss) * 0.5);    // drier tussock tops
    turf *= 0.75 + 0.5 * tuft;
    turf *= 0.85 + 0.3 * grain;

    // The forest floor: litter where the canopy closes, soil where nothing
    // grows in the open.
    float lf;
    float lshade;
    float leafSize = 0.055;
    float leafSeen = clamp(leafSize / fp * 0.4 - 0.5, 0.0, 1.0);
    vec3 litter = vec3(0.095, 0.07, 0.048);   // the leaves' mean
    float litterAo = 1.0;
    if (leafSeen > 0.0 && under > 0.02) {
        vec3 leaf = groundLeaves(xz, leafSize, lf, lshade);
        vec3 l = leaf * lshade * mix(0.7, 1.0, smoothstep(0.0, 0.15, lf));
        litter = mix(litter, l, leafSeen);
        litterAo = mix(1.0, mix(0.7, 1.0, lshade), leafSeen);
    }
    // Leaf drifts (some fresher and russet, some old and grey), damp dark
    // hollows and moss, at scales that hold from a few metres to far above.
    float drift = groundFbm(xz + 5.0, 0.25, fp, 3);
    float heap = groundFbm(xz + 23.0, 2.2, fp, 3);
    float damp = groundFbm(xz + 51.0, 0.12, fp, 3);
    float mossy = groundFbm(xz + 91.0, 0.3, fp, 3);
    litter *= 0.65 + 0.7 * drift;
    litter *= 0.7 + 0.6 * heap;
    litter = mix(litter * vec3(0.85, 0.9, 1.0), litter * vec3(1.25, 1.0, 0.75), smoothstep(0.3, 0.7, heap));
    litter = mix(litter, vec3(0.03, 0.023, 0.017), smoothstep(0.55, 0.75, damp) * 0.75);
    litter = mix(litter, vec3(0.045, 0.075, 0.018) * (0.7 + 0.6 * tuss), smoothstep(0.58, 0.72, mossy) * 0.85);
    float twig = groundTwigs(xz, 0.35, fp) * clamp(0.35 / fp * 0.1 - 0.3, 0.0, 1.0);
    litter = mix(litter, vec3(0.045, 0.032, 0.022), twig * 0.85);
    vec3 bare = soil * (0.75 + 0.5 * drift) * (0.85 + 0.3 * grain);
    vec3 floorC = mix(bare, litter, smoothstep(0.0, 0.3, under));

    color = mix(floorC, turf, cover);
    ao *= mix(litterAo, 1.0, cover);
    roughness = 1.0;
}
