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

uniform vec3  u_albedoRock;
uniform float u_roughnessRock;
uniform vec3  u_albedoSnow;
uniform float u_roughnessSnow;
uniform vec3  u_albedoSand;
uniform float u_roughnessSand;
uniform vec3  u_albedoGrass;
uniform float u_roughnessGrass;
uniform vec3  u_albedoForest;  // L0 canopy albedo (linear) for the forest tint
uniform float u_forestTint;    // 0..1 strength of the forest recolour (0 = off)

uniform sampler2D u_surface;  // surface layer: R=biome, G=moisture, B=temperature
uniform vec3 u_surfA;         // (originX, originZ, metresPerCell)
uniform vec2 u_surfB;         // (width, height)
uniform float u_surfPresent;  // 1.0 if present, 0.0 otherwise

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

    // Sample surface layer if present
    vec3 surfVal = vec3(0.0);
    if (u_surfPresent > 0.5) {
        vec2 t = (rel + u_camXZ - u_surfA.xy) / u_surfA.z;
        vec2 uv = (t + 0.5) / u_surfB;
        uv = clamp(uv, vec2(0.5 / u_surfB.x, 0.5 / u_surfB.y), vec2(1.0 - 0.5 / u_surfB.x, 1.0 - 0.5 / u_surfB.y));
        surfVal = texture(u_surface, uv).rgb;
    }

    // Rock wherever the surface is too steep to hold soil.
    float rock = smoothstep(0.10, 0.42, slope + 0.06 * coarse);

    // Snow above a jittered line, and only where it can settle: a 60-degree
    // face keeps none. Wind-scouring is what the coarse term stands in for.
    float snowLine = u_snowLine + 260.0 * coarse;
    if (u_surfPresent > 0.5) {
        // Temperature (surfVal.b) is low (cold) -> lower snow line. High -> higher snow line.
        // Assume temperature is centered around 0.3.
        snowLine += (surfVal.b - 0.3) * 1500.0;
    }
    float snow = smoothstep(snowLine, snowLine + 340.0, wy)
               * (1.0 - smoothstep(0.18, 0.52, slope));

    // Sand hugs sea level on gentle ground.
    float sand = (1.0 - smoothstep(u_seaLevel + 4.0, u_seaLevel + 34.0, wy))
               * (1.0 - smoothstep(0.06, 0.20, slope));

    float grass = clamp(1.0 - rock - snow - sand, 0.0, 1.0);

    // Spatially modulate by biome ID (surfVal.r)
    if (u_surfPresent > 0.5) {
        float b = surfVal.r;
        if (b == 5.0 || b == 8.0) {
            // Desert (cold desert / subtropical desert): convert grass to sand/rock
            sand += grass * 0.7;
            rock += grass * 0.3;
            grass = 0.0;
        } else if (b == 2.0) {
            // Ice / Tundra: convert half grass to snow
            snow += grass * 0.5;
            grass *= 0.5;
        }
    }

    float total = rock + snow + sand + grass;

    vec3 cRock  = mix(u_albedoRock, u_albedoRock * 1.36, 0.5 + 0.5 * fine);
    vec3 cSnow  = u_albedoSnow * (1.0 + 0.05 * fine);
    vec3 cSand  = mix(u_albedoSand, u_albedoSand * 1.17, 0.5 + 0.5 * fine);

    // Grass swings between dry and lush over hundreds of metres/climate:
    vec3 lushGrass = u_albedoGrass;
    vec3 dryGrass  = vec3(0.38, 0.34, 0.20); // parched savanna/desert grass
    vec3 coldGrass = vec3(0.28, 0.26, 0.22); // tundra grey-brown grass

    vec3 baseGrass = u_albedoGrass;
    if (u_surfPresent > 0.5) {
        // Blend based on moisture (surfVal.g) and temperature (surfVal.b)
        baseGrass = mix(dryGrass, lushGrass, clamp(surfVal.g * 1.5, 0.0, 1.0));
        baseGrass = mix(coldGrass, baseGrass, clamp(surfVal.b * 2.0, 0.0, 1.0));
    }
    vec3 cGrass = mix(baseGrass, baseGrass * 1.4,
                      clamp(0.5 + 0.5 * coarse + 0.18 * fine, 0.0, 1.0));

    // ---- L0 forest canopy tint ----------------------------------------------
    // Dense-forest biomes on tree-holding ground read as a darker, richer canopy
    // green straight from the terrain material — this is the FAR foliage layer,
    // so distant forest needs no billboards drawn at all. Recolours the grass
    // toward canopy where the surface layer says forest; band-limited by the
    // same coarse/fine mottle it borrows, so it breaks into clumps and dissolves
    // to its mean at range rather than aliasing into speckle.
    if (u_surfPresent > 0.5 && u_forestTint > 0.001) {
        float bf = surfVal.r;
        float forest =
            (bf > 3.5 && bf < 4.5)  ? 1.0  :   // boreal / taiga
            (bf > 6.5 && bf < 7.5)  ? 1.0  :   // temperate forest
            (bf > 9.5 && bf < 11.5) ? 1.0  :   // seasonal tropical + rainforest
            (bf > 5.5 && bf < 6.5)  ? 0.35 :   // grassland (scattered trees)
            (bf > 8.5 && bf < 9.5)  ? 0.35 :   // savanna (scattered trees)
            0.0;
        forest *= (1.0 - smoothstep(0.22, 0.5, slope));       // only where trees hold
        forest *= clamp(0.45 + 0.7 * surfVal.g, 0.0, 1.0);    // wetter -> denser
        float clump = clamp(0.6 + 0.7 * coarse + 0.3 * fine, 0.0, 1.0);
        vec3 canopy = u_albedoForest * (0.7 + 0.55 * clump);
        cGrass = mix(cGrass, canopy, forest * u_forestTint);
    }

    m.albedo = (cRock * rock + cSnow * snow + cSand * sand + cGrass * grass)
             / max(total, 1e-4);
    m.roughness = (u_roughnessRock * rock + u_roughnessSnow * snow + u_roughnessSand * sand + u_roughnessGrass * grass)
                / max(total, 1e-4);

    // Cavity: sitting below the local detail mean means less sky reaches here.
    // Cheap, and it is what gives metre-scale relief its readable shape without
    // an ambient-occlusion pass. Kept subtle on purpose — pushed hard it stops
    // reading as shadow and starts reading as blotches of dirt painted on.
    m.albedo *= mix(0.88, 1.0, smoothstep(-1.0, 0.35, cavity));
}
