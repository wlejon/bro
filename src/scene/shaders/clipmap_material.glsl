// Clipmap terrain — surface materials.
//
// Fragment-only; concatenated after clipmap_detail.glsl, whose noise and
// anchoring it reuses.
//
// The rule this replaces was a two-colour altitude ramp, which is why every
// screenshot read as tinted clay: altitude alone cannot tell a meadow from a
// cliff face, and a colour with no texture has no scale, so ground under your
// feet looked exactly like ground ten kilometres away.
//
// Materials are chosen by what the surface IS — how steep it is, how high, and
// whether it is a hollow or a ridge — and each one carries its own mottling at
// its own scale, band-limited the same way the displacement is. That last part
// is what makes the distance behave: the texture of a material stops being
// resolved before it can alias, and the surface fades to its average colour
// rather than to noise.

uniform float u_snowLine;   // world metres where snow starts, before jitter

// Mottling scales, metres. Coarse breaks up the large flat washes; fine is the
// scale you notice underfoot.
const float CM_MOTTLE_COARSE = 34.0;
const float CM_MOTTLE_FINE   = 2.3;

// Triplanar scalar mottling in roughly [-1,1], band-limited against the
// rendered cell size.
//
// Projecting on XZ alone is fine for ground but smears into vertical streaks on
// anything steep — precisely the rock faces that need texture most. The three
// planes are blended by the squared normal, so each face is textured by the
// projection that faces it.
//
// XZ rides the same anchored lattice as the displacement (see cmDetail); world
// Y is used directly, since terrain altitude never leaves fp32's comfortable
// range.
float cmMottle(vec2 rel, float wy, vec3 n, float lambda, float c) {
    float w = 1.0 - smoothstep(CM_DETAIL_FADE_LO * lambda,
                               CM_DETAIL_FADE_HI * lambda, c);
    if (w <= 0.0) return 0.0;

    float inv = 1.0 / lambda;
    vec2  q   = (rel + u_detailOffset) * inv;
    ivec2 ac  = ivec2(u_detailAnchor * inv);
    float y   = wy * inv;

    vec3 bw = n * n;                       // already normalised, so sums to 1
    float v = 0.0;
    v += bw.y * cmNoiseD(q,                ac).x;
    v += bw.z * cmNoiseD(vec2(q.x, y),     ivec2(ac.x, 0)).x;
    v += bw.x * cmNoiseD(vec2(q.y, y),     ivec2(ac.y, 0)).x;
    return w * v;
}

// A material: albedo plus how rough it is.
struct CmMaterial {
    vec3  albedo;
    float roughness;
};

// Weights sum to 1. Every threshold is jittered by the coarse mottle so that
// nothing in the world is a contour line — an unjittered snowline is the single
// most artificial thing a terrain shader can draw.
void cmMaterialAt(vec2 rel, float wy, vec3 n, float c, float cavity,
                  out CmMaterial m) {
    float coarse = cmMottle(rel, wy, n, CM_MOTTLE_COARSE, c);
    float fine   = cmMottle(rel, wy, n, CM_MOTTLE_FINE,   c);

    float slope = clamp(1.0 - n.y, 0.0, 1.0);

    // Rock wherever the surface is too steep to hold soil.
    float rock = smoothstep(0.10, 0.42, slope + 0.06 * coarse);

    // Snow above a jittered line, and only where it can settle: a 60-degree
    // face keeps none. Wind-scouring is what the coarse term stands in for.
    float snowLine = u_snowLine + 260.0 * coarse;
    float snow = smoothstep(snowLine, snowLine + 340.0, wy)
               * (1.0 - smoothstep(0.18, 0.52, slope));

    // Sand hugs sea level on gentle ground.
    float sand = (1.0 - smoothstep(u_seaLevel + 4.0, u_seaLevel + 34.0, wy))
               * (1.0 - smoothstep(0.06, 0.20, slope));

    float grass = clamp(1.0 - rock - snow - sand, 0.0, 1.0);
    float total = rock + snow + sand + grass;

    vec3 cRock  = mix(vec3(0.246, 0.232, 0.221), vec3(0.336, 0.313, 0.288),
                      0.5 + 0.5 * fine);
    vec3 cSnow  = vec3(0.760, 0.790, 0.830) * (1.0 + 0.05 * fine);
    vec3 cSand  = mix(vec3(0.480, 0.430, 0.330), vec3(0.560, 0.510, 0.400),
                      0.5 + 0.5 * fine);
    // Grass swings between dry and lush over hundreds of metres; a single green
    // over a whole continent is the other half of the "pastel painting" look.
    vec3 cGrass = mix(vec3(0.180, 0.235, 0.128), vec3(0.268, 0.322, 0.170),
                      clamp(0.5 + 0.5 * coarse + 0.18 * fine, 0.0, 1.0));

    m.albedo = (cRock * rock + cSnow * snow + cSand * sand + cGrass * grass)
             / max(total, 1e-4);
    m.roughness = (0.88 * rock + 0.62 * snow + 0.94 * sand + 0.97 * grass)
                / max(total, 1e-4);

    // Cavity: sitting below the local detail mean means less sky reaches here.
    // Cheap, and it is what gives metre-scale relief its readable shape without
    // an ambient-occlusion pass. Kept subtle on purpose — pushed hard it stops
    // reading as shadow and starts reading as blotches of dirt painted on.
    m.albedo *= mix(0.88, 1.0, smoothstep(-1.0, 0.35, cavity));
}
