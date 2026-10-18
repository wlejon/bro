// Set 1 of every lit scene pipeline and the shading every lit surface shares:
// Cook-Torrance PBR over the frame's lights, the shadow atlas, the ambient
// (split-sum image-based lighting from the environment, else the flat
// ambient) with the bound reflection probe's specular, the air between the
// eye and the surface (aerial perspective or fog) and the tile shade map. Mirrors SceneLightingUniforms (scene_vk_descriptors.h); filled by
// scene_lighting.cpp from SceneRenderer's light list and shadow plan.
// Needs scene_camera.glsl included first. Positions are camera-relative, as
// in every scene shader: the lights, shadow matrices, probe and shade-map
// origin arrive with the eye subtracted (scene_lighting.cpp).
//
//   D = GGX (Trowbridge-Reitz), F = Schlick, G = Smith (Schlick-GGX).
// Light types: 0 = directional, 1 = point, 2 = spot. Range uses the
// Epic/Frostbite smooth window, pow(saturate(1 - (d/range)^4), 2) / (d^2 + 1);
// a spot fades smoothly from its inner to its outer cone.

const float PI = 3.14159265359;

#define SCENE_MAX_LIGHTS 32
#define SCENE_MAX_SHADOW_TILES 16

struct SceneLight {
    vec4 position;      // xyz camera-relative position, w type
    vec4 direction;     // xyz unit direction (light -> scene), w range (0 = unbounded)
    vec4 color;         // rgb linear, a intensity
    vec4 shadow;        // x cos(inner), y cos(outer), z first atlas tile (-1 = none), w tile count
    vec4 cascadeSplit;  // far view distance of each cascade (directional CSM)
};

// One atlas tile. `matrix` takes a camera-relative position straight to
// (tile uv, [0,1] depth); `rect` places the tile in the atlas. Biases are in world units and
// shadow texels: bias.x a constant depth bias, bias.y the normal offset,
// bias.zw the world size of one texel (constant for ortho tiles, per metre of
// light distance for perspective ones); depth = (near, far, ortho) of the
// tile's projection, to turn a world-unit bias into [0,1] depth.
struct ShadowTile {
    mat4 matrix;
    vec4 rect;
    vec4 bias;
    vec4 depth;
};

layout(set = 1, binding = 0) uniform LightingUBO {
    vec4 sunDirection;  // the dominant directional light (decals), w = 1 when present
    vec4 sunColor;      // rgb, a = intensity
    vec4 ambientColor;  // rgb flat ambient
    vec4 params;        // x light count, y PCF grid side (1/3/5), z 1/atlas size, w shadow tile count
    SceneLight lights[SCENE_MAX_LIGHTS];
    ShadowTile shadows[SCENE_MAX_SHADOW_TILES];
    mat4 probeWorldToLocal;
    mat4 probeLocalToWorld;
    vec4 probePos;      // xyz = pos, w = enabled
    vec4 probeBoxSize;  // xyz = box size, w = box projection
    vec4 probeParams;   // x = intensity, y = blend distance, z = max LOD
    vec4 shadeOrigin;   // xyz = origin, w = has shade map
    vec4 shadeParams;   // x = cell size, y = hex, z = width, w = height
    vec4 iblParams;     // x = enabled, y = intensity, z = rotation, w = prefilter max LOD
    vec4 atmSunDir;     // xyz = towards the sun, w = atmosphere enabled
    vec4 atmSunColor;   // rgb = solar irradiance, a = planet radius
    vec4 atmBetaR;      // rgb = Rayleigh scattering, a = thickness
    vec4 atmParams;     // x = Mie scattering, y = Mie g, z/w = Rayleigh/Mie scale height
    vec4 atmParams2;    // x = sea level, y = spherical, z = multi-scatter, w = sun angular radius
    vec4 atmCenter;     // xyz = planet centre, w = sun disk intensity
    vec4 groundBounce;  // rgb = the ground's bounce, as irradiance over pi and intensity (0: none)
} lighting;

layout(set = 1, binding = 1) uniform sampler2DShadow shadowAtlas;
// Bindings 2-7 are bare images read through the one shared sampler at
// binding 8 (linear, mipmapped, clamped): a fragment stage also sees the
// material's 5 and a custom shader's 8 samplers, and Apple GPUs allow 16.
layout(set = 1, binding = 8) uniform sampler lightingSampler;
layout(set = 1, binding = 2) uniform textureCube texReflectionProbeImage;
layout(set = 1, binding = 3) uniform texture2D texShadeMapImage;
// The environment (scene/vulkan/scene_environment.h): the cosine-convolved
// irradiance, the GGX-prefiltered radiance (roughness down the mips), the
// split-sum BRDF LUT (NdotV, roughness) -> (F0 scale, bias), and the
// radiance cube the sky draws.
layout(set = 1, binding = 4) uniform textureCube texIrradianceImage;
layout(set = 1, binding = 5) uniform textureCube texPrefilterImage;
layout(set = 1, binding = 6) uniform texture2D texBrdfLutImage;
layout(set = 1, binding = 7) uniform textureCube texEnvironmentImage;
#define texReflectionProbe samplerCube(texReflectionProbeImage, lightingSampler)
#define texShadeMap sampler2D(texShadeMapImage, lightingSampler)
#define texIrradiance samplerCube(texIrradianceImage, lightingSampler)
#define texPrefilter samplerCube(texPrefilterImage, lightingSampler)
#define texBrdfLut sampler2D(texBrdfLutImage, lightingSampler)
#define texEnvironment samplerCube(texEnvironmentImage, lightingSampler)

#include "scene_atmosphere.glsl"

// `d` turned about +Y by `a` radians: the environment's rotation.
vec3 rotateY(vec3 d, float a) {
    float c = cos(a), s = sin(a);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Lagarde's roughness-aware Fresnel, for ambient terms with no half vector.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distanceAttenuation(float dist, float range) {
    if (range <= 0.0) return 1.0;
    float t = dist / range;
    float win = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return win * win / (dist * dist + 1.0);
}

// One atlas tile: 1 lit, 0 shadowed, 1 outside the tile's frustum. The filter
// is a grid of hardware-bilinear compares, params.y taps on a side at
// `spread`-texel spacing (3x3 at 1 is a 4-texel tent), clamped inside the
// tile so a neighbour never bleeds in.
float sampleShadow(int slot, vec3 pos, float biasWorld, float spread) {
    ShadowTile tile = lighting.shadows[slot];
    vec4 sc = tile.matrix * vec4(pos, 1.0);
    if (sc.w <= 0.0) return 1.0;
    float zEye = sc.w;
    sc /= sc.w;
    if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0) return 1.0;
    float span = max(tile.depth.y - tile.depth.x, 1e-6);
    float b01 = tile.depth.z > 0.5 ? biasWorld / span
                                   : biasWorld * tile.depth.x * tile.depth.y / (span * zEye * zEye);
    float ref = sc.z - tile.bias.x - b01;
    vec2 texel = vec2(lighting.params.z);
    vec2 base = tile.rect.xy + sc.xy * tile.rect.zw;
    int hk = (int(lighting.params.y) - 1) / 2;
    vec2 minUV = tile.rect.xy + texel * (float(hk) + 1.0);
    vec2 maxUV = tile.rect.xy + tile.rect.zw - texel * (float(hk) + 1.0);
    // spread 0: the one hardware-filtered tap.
    if (hk <= 0 || spread == 0.0) return texture(shadowAtlas, vec3(clamp(base, minUV, maxUV), ref));
    float s = 0.0;
    // spread < 0: four hardware-filtered taps spanning about what the grid
    // at -spread does, at under half its cost.
    if (spread < 0.0) {
        vec2 o = texel * (-spread * 0.5 * float(hk));
        s = texture(shadowAtlas, vec3(clamp(base + vec2(-o.x, -o.y), minUV, maxUV), ref)) +
            texture(shadowAtlas, vec3(clamp(base + vec2(o.x, -o.y), minUV, maxUV), ref)) +
            texture(shadowAtlas, vec3(clamp(base + vec2(-o.x, o.y), minUV, maxUV), ref)) +
            texture(shadowAtlas, vec3(clamp(base + vec2(o.x, o.y), minUV, maxUV), ref));
        return s * 0.25;
    }
    vec2 step = texel * spread;
    for (int y = -hk; y <= hk; ++y) {
        for (int x = -hk; x <= hk; ++x) {
            s += texture(shadowAtlas, vec3(clamp(base + vec2(x, y) * step, minUV, maxUV), ref));
        }
    }
    float side = float(2 * hk + 1);
    return s / (side * side);
}

// The shadow term of tile `slot`: the lookup point pushed off the surface
// along the normal and a slope-scaled depth bias, both sized in shadow texels
// (about half a texel head-on, more at grazing incidence). The caster pass
// culls light-facing faces and applies a slope offset; this is the remainder.
float shadowFromTile(int slot, vec3 pos, float lightDist, vec3 N, float NdotL, float spread) {
    ShadowTile tile = lighting.shadows[slot];
    float texelW = tile.bias.z + tile.bias.w * lightDist;
    float sinT = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    float nOff = tile.bias.y + texelW * (0.5 + sinT);
    float bWorld = texelW * (0.3 + 0.5 * sinT);
    return sampleShadow(slot, pos + N * nOff, bWorld, spread);
}

// Light `i`'s shadow at `pos`: the cube face of a point light, the cascade of
// a directional one (blended into the next across the last 15% of each), or
// the single tile of anything else. `spread` widens the filter (1: a 4-texel
// tent at pcf 3; 0: one hardware-filtered tap).
float lightShadow(int i, vec3 pos, float camDist, float lightDist, vec3 N, float NdotL, float spread) {
    SceneLight light = lighting.lights[i];
    int slot = int(light.shadow.z);
    if (slot < 0) return 1.0;
    int count = int(light.shadow.w);
    int type = int(light.position.w);
    if (type == 1 && count == 6) {
        vec3 toFrag = pos - light.position.xyz;
        vec3 a = abs(toFrag);
        int face;
        if (a.x >= a.y && a.x >= a.z) face = toFrag.x > 0.0 ? 0 : 1;
        else if (a.y >= a.z)          face = toFrag.y > 0.0 ? 2 : 3;
        else                          face = toFrag.z > 0.0 ? 4 : 5;
        return shadowFromTile(slot + face, pos, lightDist, N, NdotL, spread);
    }
    if (type == 0 && count > 1) {
        vec4 splits = light.cascadeSplit;
        int c = 0;
        if (count >= 2 && camDist > splits.x) c = 1;
        if (count >= 3 && camDist > splits.y) c = 2;
        if (count >= 4 && camDist > splits.z) c = 3;
        float thisFar = splits[c];
        float prevFar = c == 0 ? 0.0 : splits[c - 1];
        float shadow = shadowFromTile(slot + c, pos, lightDist, N, NdotL, spread);
        if (c < count - 1) {
            float blendStart = thisFar - max(thisFar - prevFar, 1e-4) * 0.15;
            float tb = clamp((camDist - blendStart) / max(thisFar - blendStart, 1e-4), 0.0, 1.0);
            if (tb > 0.0) shadow = mix(shadow, shadowFromTile(slot + c + 1, pos, lightDist, N, NdotL, spread), tb);
        }
        return shadow;
    }
    return shadowFromTile(slot, pos, lightDist, N, NdotL, spread);
}

// A lit surface, as every lit shader hands it to sceneDirectLight/sceneAmbient.
struct SceneSurface {
    vec3 position;     // camera-relative
    vec3 normal;       // unit shading normal
    vec3 view;         // unit, surface -> eye
    float camDist;
    vec3 baseColor;
    float metallic;
    float roughness;   // clamped to [0.04, 1]
    bool receivesShadow;
    bool twoSided;
    float subsurface;  // wrap-light translucency, two-sided surfaces only
    float translucency; // leaf light transport: the sun glowing through a leaf seen against it (0: none)
    // A thin surface's own face toward the eye, which the light through it
    // follows when its shading normal is another (a grass blade lit as the
    // ground it covers); zero: the shading normal.
    vec3 thinNormal;
    // Leaves placed in a sphere (a forest lobe, a crown's clump, or the lobe
    // a cluster of thinned leaves belongs to; scene_forest_mass.glsl):
    // its centre (camera-relative) and radius, w 0 for none, and the leaves'
    // optical depth a unit length through it. Such a leaf is shadowed by its
    // own sphere's leaves analytically, and by everything else from the
    // shadow map where the light enters the sphere (past the sphere's own
    // caster, shadow_forest_sprite.vert).
    // leafPath: the light's path through those leaves, times (how many lie
    // in it wanders from pixel to pixel).
    vec4 leafSphere;
    float leafDensity;
    float leafPath;
    // The sphere's height over its width (a conifer's flat lobes): the light
    // leaves an ellipsoid, so a flat pad lit from above is lit through its
    // thickness, not its breadth. 1: a sphere.
    float leafFlat;
    // Placed leaves (forest lobes, crowns and clusters) know the share of
    // the sun reaching where they lie, not which of their leaves it reaches:
    // a leaf is lit or in another's shadow, so the leaves of a crown are
    // sunflecks among shade, which the leaves tier shows leaf by leaf and,
    // under MSAA, a pixel as the mean of its samples' leaves. leafSun holds a
    // draw in [0, 1) for each of up to four leaves seen in the pixel, each lit
    // when its draw is under the share; leafSunN is how many of them the
    // pixel averages (1 to 4, fractional between; 0 keeps the share as is).
    vec4 leafSun;
    float leafSunN;
    // Placed leaves are lit through one normal a pixel, but the leaves it
    // stands for face many ways about it: their diffuse light is the wrap's
    // mean over normals whose cosine to the light spreads this far either
    // side of the one given (0: as given).
    float leafSpread;
};

// A leaf's wrapped diffuse, clamp((c + 0.3) / 1.3, 0, 1), averaged over
// cosines c spread evenly `k` either side of `c` (k 0: the wrap itself).
float leafWrapIntegral(float x) {
    float t = clamp(x + 0.3, 0.0, 1.3);
    return t * t / 2.6 + max(x - 1.0, 0.0);
}
float leafWrapMean(float c, float k) {
    if (k < 1e-3) return clamp((c + 0.3) / 1.3, 0.0, 1.0);
    return (leafWrapIntegral(c + k) - leafWrapIntegral(c - k)) / (2.0 * k);
}

// The sun on a pixel of placed leaves (SceneSurface::leafSun): each of its
// leaves lit or not by its draw, the share `vis` on average.
float leafSunflecks(vec4 draw, float n, float vis) {
    if (n <= 0.0) return vis;
    vec4 lit = smoothstep(draw - 0.08, draw + 0.08, vec4(vis));
    vec4 w = clamp(vec4(n) - vec4(0.0, 1.0, 2.0, 3.0), 0.0, 1.0);
    float flecks = dot(lit, w) / max(dot(w, vec4(1.0)), 1e-4);
    return mix(vis, flecks, min(n, 1.0));
}

// The direct light of every scene light on `s`.
vec3 sceneDirectLight(SceneSurface s) {
    vec3 N = s.normal;
    vec3 V = s.view;
    float NdotV = max(dot(N, V), 1e-4);
    vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
    vec3 Lo = vec3(0.0);
    int count = int(lighting.params.x);
    for (int i = 0; i < count && i < SCENE_MAX_LIGHTS; ++i) {
        SceneLight light = lighting.lights[i];
        int type = int(light.position.w);
        vec3 L;
        float atten = 1.0;
        float lightDist = 0.0;
        if (type == 0) {
            L = -light.direction.xyz;
        } else {
            vec3 toLight = light.position.xyz - s.position;
            float d = length(toLight);
            if (d < 1e-4) continue;
            lightDist = d;
            L = toLight / d;
            atten = distanceAttenuation(d, light.direction.w);
            if (type == 2) {
                float c = dot(-L, light.direction.xyz);
                float t = clamp((c - light.shadow.y) / max(light.shadow.x - light.shadow.y, 1e-4), 0.0, 1.0);
                atten *= t * t * (3.0 - 2.0 * t);
            }
            if (atten <= 0.0) continue;
        }

        vec3 radiance = light.color.rgb * light.color.a * atten;
        // Wrap light through thin two-sided surfaces (leaves): lights the back
        // relative to L whether or not the front faces the light.
        if (s.twoSided && s.subsurface > 0.0 && s.translucency <= 0.0) {
            float wrap = max(0.0, dot(-N, L) + s.subsurface) * s.subsurface;
            Lo += s.baseColor * radiance * wrap * 0.6;
        }
        // A leaf: one shadow lookup serves the light through it and on it,
        // taken on the side facing the light with a filter twice as wide (a
        // leaf's shadow on the next one is soft: the sun's disc blurs it by
        // about 1 cm a metre between them), and never quite black (a leaf
        // passes on about a tenth of what reaches it).
        bool leafy = s.translucency > 0.0;
        float NL = dot(N, L);
        float leafShadow = 1.0;
        if (leafy && s.receivesShadow) {
            vec3 tN = NL < 0.0 ? -N : N;
            vec3 at = s.position;
            // Four hardware-filtered taps as wide as nine point taps: leaves
            // are most of the frame's lit pixels, and a leaf's shadow is
            // soft (the sun's disc) whichever way it is filtered.
            float self = 1.0, spread = -2.0;
            if (s.leafSphere.w > 0.0) {
                // Out of the sphere toward the light, through its leaves;
                // what falls on it from outside is coarse (the sphere's own
                // leaves are the fine part), so one tap.
                vec3 oc = at - s.leafSphere.xyz;
                float ky = clamp(s.leafFlat, 0.1, 1.0);
                oc.y /= ky;
                vec3 Ls = vec3(L.x, L.y / ky, L.z);
                float la = dot(Ls, Ls), b = dot(oc, Ls);
                float out_ = max((-b + sqrt(max(b * b - la * (dot(oc, oc) - s.leafSphere.w * s.leafSphere.w), 0.0))) / la, 0.0);
                at += L * out_;
                self = exp(-s.leafDensity * s.leafPath * out_);
                // The exit tap is as soft as a leaf's (twice the filter):
                // sharp, the discs' cells and the wider casters' edges read
                // as hard blotches on the lobes they fall on.
            }
            float vis = self * lightShadow(i, at, s.camDist, lightDist, tN, max(abs(NL), 0.2), spread);
            leafShadow = 0.1 + 0.9 * (type == 0 ? leafSunflecks(s.leafSun, s.leafSunN, vis) : vis);        }
        // Leaf translucency: light passing through a leaf lit from behind,
        // strongest looking toward the light, a warm yellow-green, and
        // shadowed by the leaves between it and the light.
        if (leafy) {
            float NLt = dot(s.thinNormal, s.thinNormal) > 0.25 ? dot(s.thinNormal, L) : NL;
            float through = smoothstep(-0.3, 0.4, 0.3 - NLt);
            float back = pow(max(dot(-V, L), 0.0), 3.0);
            float amount = s.translucency * through * (0.1 + 0.9 * back);
            vec3 tcol = sqrt(max(s.baseColor, vec3(0.0))) * vec3(0.75, 0.95, 0.35) * 0.55;
            Lo += tcol * radiance * amount * leafShadow;
        }

        // A leaf's diffuse light wraps a little past its edge-on angle, so a
        // leaf turning away from the sun dims rather than switching off.
        float NdotL = max(NL, 0.0);
        float diffNL = leafy ? leafWrapMean(NL, s.leafSpread) : NdotL;
        if (diffNL <= 0.0) continue;
        vec3 H = normalize(V + L);
        float D = distributionGGX(max(dot(N, H), 0.0), s.roughness);
        float G = geometrySmith(NdotV, NdotL, s.roughness);
        vec3 F = fresnelSchlick(max(dot(V, H), 0.0), F0);
        vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
        vec3 diffuse = (vec3(1.0) - F) * (1.0 - s.metallic) * s.baseColor / PI;

        float shadow = leafy ? leafShadow
                     : s.receivesShadow ? lightShadow(i, s.position, s.camDist, lightDist, N, NdotL, 1.0) : 1.0;
        Lo += shadow * (diffuse * diffNL + specular * NdotL) * radiance;
    }
    return Lo;
}

// The bound reflection probe's specular along R, and its weight at `pos`: 1
// inside the box past the interior margin, fading to 0 at the faces. With box
// projection the reflection ray is intersected with the box and the capture
// sampled toward the hit point (parallax correction).
vec3 probeRadiance(vec3 pos, vec3 R, float rough, out float w) {
    vec3 lp = (lighting.probeWorldToLocal * vec4(pos, 1.0)).xyz;
    vec3 edgeDist = (vec3(0.5) - abs(lp)) * lighting.probeBoxSize.xyz;
    float dmin = min(min(edgeDist.x, edgeDist.y), edgeDist.z);
    float blendDist = lighting.probeParams.y;
    w = blendDist > 0.0 ? clamp(dmin / blendDist, 0.0, 1.0) : step(0.0, dmin);
    vec3 dir = R;
    if (lighting.probeBoxSize.w > 0.5) {
        vec3 ld = mat3(lighting.probeWorldToLocal) * R;
        ld = mix(ld, vec3(1e-6), vec3(lessThan(abs(ld), vec3(1e-6))));
        vec3 invD = 1.0 / ld;
        vec3 tm = max((vec3(-0.5) - lp) * invD, (vec3(0.5) - lp) * invD);
        float tHit = min(min(tm.x, tm.y), tm.z);
        vec3 hit = (lighting.probeLocalToWorld * vec4(lp + ld * tHit, 1.0)).xyz;
        vec3 d2 = hit - lighting.probePos.xyz;
        if (dot(d2, d2) > 1e-8) dir = d2;
    }
    return textureLod(texReflectionProbe, dir, rough * lighting.probeParams.z).rgb;
}

// Ambient (indirect) light on `s`, Karis' split sum: with an environment the
// irradiance on the diffuse part and the prefiltered radiance times the BRDF
// LUT on the specular, both turned by the environment's rotation; without
// one the flat ambient on the diffuse part. A bound reflection probe replaces
// the global specular by its weight at the surface (probes are specular-only).
vec3 sceneAmbient(SceneSurface s) {
    // Leaves are lit through their crown's bulk normal, which turns edge-on
    // at the crown's outline; their own faces lie every way, so the sky they
    // mirror is a mean over angles, never the grazing sheen a solid's rim
    // takes (which paled a dark conifer against the sky).
    float NdotV = max(dot(s.normal, s.view), s.translucency > 0.0 ? 0.6 : 1e-4);
    vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, s.roughness);
    vec3 R = reflect(-s.view, s.normal);
    vec2 brdf = texture(texBrdfLut, vec2(NdotV, s.roughness)).rg;

    float probeW = 0.0;
    vec3 probeSpec = vec3(0.0);
    if (lighting.probePos.w > 0.5) {
        vec3 raw = probeRadiance(s.position, R, s.roughness, probeW);
        probeSpec = raw * (F * brdf.x + brdf.y) * lighting.probeParams.x;
    }
    if (lighting.iblParams.x > 0.5) {
        float rotation = lighting.iblParams.z;
        float intensity = lighting.iblParams.y;
        vec3 kD = (1.0 - F) * (1.0 - s.metallic);
        vec3 irradiance = texture(texIrradiance, rotateY(s.normal, rotation)).rgb;
        // The ground's bounce (SceneRenderer::setGroundBounce): the light a
        // level ground sends back, sun and sky on it (worked out once a frame,
        // SceneEnvironment::fillLighting), seen by the surface's lower
        // hemisphere ((1 - n.y) / 2 of it is ground).
        irradiance += lighting.groundBounce.rgb * (0.5 - 0.5 * s.normal.y);
        vec3 prefiltered = textureLod(texPrefilter, rotateY(R, rotation), s.roughness * lighting.iblParams.w).rgb;
        vec3 specular = prefiltered * (F * brdf.x + brdf.y);
        return kD * irradiance * s.baseColor * intensity + mix(specular * intensity, probeSpec, probeW);
    }
    return lighting.ambientColor.rgb * s.baseColor * (1.0 - s.metallic) + probeSpec * probeW;
}

// Fog in [0,1] for a fragment `camDist` from the eye at world height `worldY`:
// exponential-squared height fog when the density is set, else the linear
// start/end ramp, else none.
float fogFactorFor(float camDist, float worldY) {
    float density = camera.fogParams.z;
    if (density > 0.0) {
        float d = max(camDist - camera.fogParams.w, 0.0);
        float falloff = camera.fogColor.a;
        if (falloff > 0.0) density *= exp(-falloff * worldY);
        float x = density * d;
        return 1.0 - exp(-x * x);
    }
    float fogStart = camera.fogParams.x;
    float fogEnd = camera.fogParams.y;
    if (fogEnd > 0.0) {
        float f = clamp((camDist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
        return f * f;
    }
    return 0.0;
}

// The air between the eye and a surface at camera-relative `pos`, `camDist` away, as what
// survives of the surface's colour and what the air adds: the atmosphere's
// transmittance and in-scatter (aerial perspective, integrated with the sky's
// own model, so distant ground fades into the sky behind it) when it is on,
// else the fog. Aerial perspective supersedes fog; both model the same air.
// `fade` is how far the surface's alpha fades out (fog only).
struct SceneAir {
    vec3 transmittance;
    vec3 inscatter;
    float fade;
};

// Aerial-perspective march: steps grow with the ray (one per 3 km) up to the
// sky pass's count, so a far ridge resolves like the sky just above it.
const int ATM_AERIAL_MIN_STEPS = 4;
const int ATM_AERIAL_MAX_STEPS = 24;
const int ATM_AERIAL_SUN_STEPS = 4;
const float ATM_AERIAL_STEP_LENGTH = 3000.0;

SceneAir sceneAir(vec3 pos, float camDist) {
    SceneAir air;
    air.transmittance = vec3(1.0);
    air.inscatter = vec3(0.0);
    air.fade = 0.0;
    if (lighting.atmSunDir.w > 0.5) {
        float dist = length(pos);
        if (dist <= 0.0) return air;
        int steps = int(clamp(dist / ATM_AERIAL_STEP_LENGTH, float(ATM_AERIAL_MIN_STEPS),
                              float(ATM_AERIAL_MAX_STEPS)));
        air.inscatter = atmScatter(atmOrigin(camera.eyeWorld.xyz), pos / dist, dist, steps, ATM_AERIAL_SUN_STEPS,
                                   air.transmittance);
        return air;
    }
    float fog = fogFactorFor(camDist, pos.y + camera.eyeWorld.y);
    air.transmittance = vec3(1.0 - fog);
    air.inscatter = camera.fogColor.rgb * fog;
    air.fade = fog;
    return air;
}

// The tile shade map's value under `pos` (see scene/shade_map.h): square
// cells by floor division, pointy-top odd-r hex cells by cube rounding. The
// point is nudged behind the surface along the geometric normal so a wall on
// a cell edge reads the cell behind its face. 1 off the grid.
float cellShade(vec3 pos, vec3 geomNormal) {
    float R = max(lighting.shadeParams.x, 1e-6);
    vec3 nudged = pos - geomNormal * (0.05 * R);
    vec2 p = nudged.xz - lighting.shadeOrigin.xz;
    int cx, cy;
    if (lighting.shadeParams.y > 0.5) {
        float r = p.y / (1.5 * R);
        float q = p.x / (1.7320508 * R) - r * 0.5;
        float x = q, z = r, y = -x - z;
        float rx = floor(x + 0.5), ry = floor(y + 0.5), rz = floor(z + 0.5);
        float dx = abs(rx - x), dy = abs(ry - y), dz = abs(rz - z);
        if (dx > dy && dx > dz) rx = -ry - rz;
        else if (dy > dz)       ry = -rx - rz;
        else                    rz = -rx - ry;
        int hq = int(rx), hr = int(rz);
        cy = hr;
        cx = hq + (hr - (hr & 1)) / 2;
    } else {
        cx = int(floor(p.x / R));
        cy = int(floor(p.y / R));
    }
    if (cx < 0 || cy < 0 || cx >= int(lighting.shadeParams.z) || cy >= int(lighting.shadeParams.w)) return 1.0;
    return texelFetch(texShadeMap, ivec2(cx, cy), 0).r;
}
