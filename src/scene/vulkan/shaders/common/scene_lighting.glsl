// Set 1 of every lit scene pipeline and the shading every lit surface shares:
// Cook-Torrance PBR over the frame's lights, the shadow pages, the ambient
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

struct SceneLight {
    vec4 position;      // xyz camera-relative position, w type
    vec4 direction;     // xyz unit direction (light -> scene), w range (0 = unbounded)
    vec4 color;         // rgb linear, a intensity
    vec4 shadow;        // x cos(inner), y cos(outer), z first shadow map (-1 = none), w map count
    vec4 shadowReach;   // x how far from the eye it shadows (0: no limit)
};

// The shadow maps (ShadowMapGpu): the sun's clipmap levels and the local
// lights' maps, each a set of cached pages in the one pool.
#include "common/scene_shadow_pages.glsl"
#include "common/scene_atmosphere_params.glsl"
#include "lighting/scene_probe_grid.glsl"
#include "ground/land_sun_layout.h"

// The land's sun (scene/world/land_sun.h): where each level lies, relative
// to the eye, and the copy shown.
struct LandSunGpu {
    vec4 params;               // x 1 when any level is shown, y the eye's world height
    vec4 body;                 // a round body's: xy the eye's world x and z, z 1 / (2 R); 0 flat
    vec4 level[LS_LEVELS];     // xy its corner (x, z), z its texel (m), w the layer shown (-1: none)
};

layout(set = 1, binding = 0) uniform LightingUBO {
    vec4 sunDirection;  // the dominant directional light (decals, the probe grid's sun), w its index + 1 (0: none)
    vec4 sunColor;      // rgb, a = intensity
    vec4 ambientColor;  // rgb flat ambient
    vec4 params;        // x light count, y PCF grid side (1/3/5), z 1/the pool's side (texels), w pool columns
    vec4 shadowView;    // x metres a pixel a metre (perspective), y metres a pixel (ortho), z finest texel, w the sway level (-1: no overlays this frame)
    vec4 shadowSoft;    // x the sun's penumbra a metre along the light, y the still penumbra (m), z the search level (-1: none)
    SceneLight lights[SCENE_MAX_LIGHTS];
    ShadowMapGpu shadowMaps[SP_MAX_MAPS];
    mat4 probeWorldToLocal;
    mat4 probeLocalToWorld;
    vec4 probePos;      // xyz = pos, w = enabled
    vec4 probeBoxSize;  // xyz = box size, w = box projection
    vec4 probeParams;   // x = intensity, y = blend distance, z = max LOD
    vec4 shadeOrigin;   // xyz = origin, w = has shade map
    vec4 shadeParams;   // x = cell size, y = hex, z = width, w = height
    vec4 iblParams;     // x = enabled, y = intensity, z = rotation, w = prefilter max LOD
    vec4 groundBounce;  // rgb = the ground's bounce, as irradiance over pi and intensity (0: none)
    AtmosphereGpu atm;  // the atmosphere (scene/vulkan/lighting/scene_atmosphere.h), atm.sunDir.w = on
    ProbeGridGpu probeGrid;   // the probe grid (scene/vulkan/lighting/scene_probe_grid.h), params.x = on
    LandSunGpu landSun;       // the land's sun, params.x = shown
} lighting;

// The pool of shadow pages and the frame's overlays (the scratch target the
// shadow pass drew what moves into), depth compared through binding 16's
// sampler; and the page table, two words a virtual page: SP_RESIDENT plus
// its physical page when it holds depth, SP_RESIDENT plus its overlay's
// place in the scratch target when what moves was drawn over it.
layout(set = 1, binding = 1) uniform texture2D shadowPoolImage;
layout(set = 1, binding = 15) uniform texture2D shadowOverlayImage;
layout(set = 1, binding = 16) uniform samplerShadow shadowCompare;
layout(std430, set = 1, binding = 14) readonly buffer ShadowPageTable {
    uint shadowTable[];
};
#define shadowPool sampler2DShadow(shadowPoolImage, shadowCompare)
#define shadowOverlay sampler2DShadow(shadowOverlayImage, shadowCompare)
// Bindings 2-7 are bare images read through the one shared sampler at
// binding 8 (linear, mipmapped, clamped): a fragment stage also sees the
// material's 5 and a custom shader's 8 samplers, and Apple GPUs allow 16.
layout(set = 1, binding = 8) uniform sampler lightingSampler;
layout(set = 1, binding = 2) uniform textureCube texReflectionProbeImage;
layout(set = 1, binding = 3) uniform texture2D texShadeMapImage;
// The environment (scene/vulkan/lighting/scene_environment.h): the cosine-convolved
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
// The atmosphere's tables (scene/vulkan/lighting/scene_atmosphere.h), each for
// a sun of 1: the transmittance (mu, r), the sky view at the eye, the aerial
// perspective's in-scatter and transmittance over the camera's frustum, and
// the sky's light as SH by (mu_s, altitude), five layers of four.
layout(set = 1, binding = 9) uniform texture2D texAtmTransmittanceImage;
layout(set = 1, binding = 10) uniform texture2D texAtmSkyViewImage;
layout(set = 1, binding = 11) uniform texture3D texAtmInscatterImage;
layout(set = 1, binding = 12) uniform texture3D texAtmAirTransmittanceImage;
layout(set = 1, binding = 13) uniform texture3D texAtmSkyLightImage;
#define texAtmTransmittance sampler2D(texAtmTransmittanceImage, lightingSampler)
#define texAtmSkyView sampler2D(texAtmSkyViewImage, lightingSampler)
#define texAtmInscatter sampler3D(texAtmInscatterImage, lightingSampler)
#define texAtmAirTransmittance sampler3D(texAtmAirTransmittanceImage, lightingSampler)
#define texAtmSkyLight sampler3D(texAtmSkyLightImage, lightingSampler)

#define ATM lighting.atm
#define ATM_TRANSMITTANCE_TEX texAtmTransmittance
#define ATM_SKYLIGHT_TEX texAtmSkyLight
#include "common/scene_atmosphere.glsl"

// The probe grid (scene/vulkan/lighting/scene_probe_grid.h): its probes' SH,
// a colour channel each, through binding 17's sampler (which repeats across:
// the columns wrap), the ground's height under its cascades and the sun's
// volume (probe_sun.comp's: g the land's shade).
layout(set = 1, binding = 17) uniform sampler probeGridSampler;
layout(set = 1, binding = 18) uniform texture3D probeShRImage;
layout(set = 1, binding = 19) uniform texture3D probeShGImage;
layout(set = 1, binding = 20) uniform texture3D probeShBImage;
layout(set = 1, binding = 21) uniform texture2D probeHeightImage;
layout(set = 1, binding = 22) uniform texture3D probeSunImage;
#define PG lighting.probeGrid
#define PG_HEIGHT_LOAD(texel) texelFetch(sampler2D(probeHeightImage, lightingSampler), texel, 0).r
#include "lighting/scene_probe_grid.glsl"

// The land's sun (scene/world/land_sun.h; land_sun.comp's): each level's
// two copies, a texel the world height a point must clear to see the sun
// and how far off what shades it stands.
layout(set = 1, binding = 23) uniform texture2DArray landSunImage;

// Whether the pipeline was built for a frame with the atmosphere on (the
// mesh drawer's kAtmosphere raster bit): without it the atmosphere's code is
// not in the shader at all, so a scene that does not use it pays nothing.
// A pipeline that does not set it keeps the branch on the uniform.
layout(constant_id = 2) const bool SCENE_ATMOSPHERE = true;

bool sceneAtmosphereOn() { return SCENE_ATMOSPHERE && lighting.atm.sunDir.w > 0.5; }

// The sun's light at camera-relative `pos`, for a sun of 1 at the top of
// the air: what the air and the planet's shadow leave of it.
vec3 sceneSunTransmittance(vec3 pos) {
    vec3 p = atmOrigin(pos);
    float r = length(p);
    return atmSunTransmittance(r,dot(p, lighting.atm.sunDir.xyz) / r);
}

// The sky's irradiance over pi on a surface at `pos` facing `n` (the
// atmosphere's ambient: the sky above, the lit ground below, through the air).
vec3 sceneSkyIrradiance(vec3 pos, vec3 n) {
    vec3 c[6];
    mat3 frame;
    atmSkyLightSh(pos, c, frame);
    return atmShIrradiance(c, frame, n);
}

// What the scene's HDR target holds at most (RGBA16F's largest finite value,
// rounded down): a lit surface's output is clamped to it, so a highlight of
// a sun in lux saturates rather than turning the pixel infinite.
const float SCENE_HDR_MAX = 64000.0;

// `d` turned about +Y by `a` radians: the environment's rotation.
vec3 rotateY(vec3 d, float a) {
    float c = cos(a), s = sin(a);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

// The open sky's irradiance over pi on a surface at `pos` facing `n`, as if
// nothing stood about it: the atmosphere's sky light, else the environment's
// irradiance with the ground's bounce below, else the flat ambient.
vec3 sceneOpenSkyIrradiance(vec3 pos, vec3 n) {
    if (sceneAtmosphereOn()) return sceneSkyIrradiance(pos, n);
    if (lighting.iblParams.x > 0.5) {
        vec3 e = texture(texIrradiance, rotateY(n, lighting.iblParams.z)).rgb;
        // The ground's bounce (SceneRenderer::setGroundBounce): the light a
        // level ground sends back, sun and sky on it (worked out once a frame,
        // SceneEnvironment::fillLighting), seen by the surface's lower
        // hemisphere ((1 - n.y) / 2 of it is ground).
        return (e + lighting.groundBounce.rgb * (0.5 - 0.5 * n.y)) * lighting.iblParams.y;
    }
    return lighting.ambientColor.rgb;
}

// The probe grid at camera-relative `pos`, from one walk over its cascades
// (the finest holding it, blended into the next coarser over the outer
// columns of its square, the ground under it read once a cascade): its SH
// (a texel a colour channel), its land shade (its sun volume's g, 1 where
// the grid is off or ends) and the spacing of its finest cascade there (m;
// what is much larger than it, the probes resolve; 0 where off or ended).
// Each lit shader reads it once a pixel and hands it to what lights it.
struct SceneProbe {
    bool on;   // the grid reaches pos
    vec4 r, g, b;
    float land;
    float spacing;
};
SceneProbe sceneProbe(vec3 pos) {
    SceneProbe p;
    p.on = false;
    p.r = p.g = p.b = vec4(0.0);
    p.land = 1.0;
    p.spacing = 0.0;
    if (PG.params.x < 0.5) return p;
    int c = pgCascadeAt(pos.xz, 0);
    if (c < 0) return p;
    p.on = true;
    p.spacing = PG.cascade[c].z;
    p.land = 0.0;
    float left = 1.0;
    for (int n = 0; n < 2 && c < pgCascades(); ++n, ++c) {
        vec2 col = (pos.xz - PG.cascade[c].xy) * PG.cascade[c].w;
        float edge = min(min(col.x, col.y), min(float(PG_COLUMNS) - col.x, float(PG_COLUMNS) - col.y));
        float w = c + 1 < pgCascades() ? left * clamp((edge - 1.0) / 3.0, 0.0, 1.0) : left;
        if (n == 1) w = left;
        if (w <= 0.0) continue;
        float h = pos.y - pgBase(pgGround(c, pos.xz));
        vec3 uvw = pgProbeUvw(c, pos.xz, h);
        p.r += w * textureLod(sampler3D(probeShRImage, probeGridSampler), uvw, 0.0);
        p.g += w * textureLod(sampler3D(probeShGImage, probeGridSampler), uvw, 0.0);
        p.b += w * textureLod(sampler3D(probeShBImage, probeGridSampler), uvw, 0.0);
        p.land += w * textureLod(sampler3D(probeSunImage, probeGridSampler), pgVolumeUvw(c, pos.xz, h), 0.0).g;
        left -= w;
        if (left <= 0.0) break;
    }
    p.land += left;
    return p;
}

// The land's sun at camera-relative `pos`: how much of the sun the land
// (and the canopy past the plants' cast range) leaves there, from the
// finest level whose square holds it, blended into the next over its last
// LS_BLEND texels; 1 past them all. A point clears what shades it over a
// height that widens with how far off that stands (the sun's disc,
// widened as the probe grid's is), and its own ground clears the level's
// heights by a share of a texel (a level's heights are bilinear between
// its texels; the ground between them is not). The levels' heights are
// over the datum: on a round body a point's world y falls below its height
// by its distance from the place origin squared over 2R.
const float LS_BLEND = 24.0;
float landSunLevels(vec3 pos) {
    if (lighting.landSun.params.x < 0.5) return 1.0;
    vec2 at = lighting.landSun.body.xy + pos.xz;
    float y = pos.y + lighting.landSun.params.y + dot(at, at) * lighting.landSun.body.z;
    float sun = 0.0, left = 1.0;
    for (int l = 0; l < LS_LEVELS; ++l) {
        vec4 lv = lighting.landSun.level[l];
        if (lv.w < 0.0) continue;
        vec2 g = (pos.xz - lv.xy) / lv.z;
        float edge = min(min(g.x, g.y), min(float(LS_SIDE) - g.x, float(LS_SIDE) - g.y));
        if (edge < 1.0) continue;
        float w = left * clamp((edge - 1.0) / LS_BLEND, 0.0, 1.0);
        vec2 s = textureLod(sampler2DArray(landSunImage, lightingSampler), vec3(g / float(LS_SIDE), lv.w), 0.0).rg;
        float soft = 0.25 * lv.z + 0.02 * s.y;
        sun += w * smoothstep(s.x - soft, s.x + soft, y + 0.1 * lv.z);
        left -= w;
        if (left <= 0.0) break;
    }
    return sun + left;
}

// What the land leaves of the sun at camera-relative `pos`: the hills'
// shade (and a canopy's past the plants' cast range), however far off they
// stand and however low the sun (the shadow pages' depth offsets swallow a
// slope that runs along a low sun's rays, and end at their reach). The
// lesser of the land's sun and the probe grid's (`probe`, read at `pos`),
// which see the same hills: the grid's ground is every mesh's that grounds
// it, the land's sun the land node's alone.
float sceneLandSun(vec3 pos, SceneProbe probe) { return min(landSunLevels(pos), probe.land); }

// How much of the open sky's light a direction keeps among what stands about
// (a specular's occlusion): the probes' irradiance facing it over the open
// sky's, by luminance, at most 1.
float sceneSkySeen(vec3 probed, vec3 open) {
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    return clamp(dot(probed, lum) / max(dot(open, lum), 1e-6), 0.0, 1.0);
}

// The ambient irradiance over pi on a surface at `pos` facing `n`: the
// probe grid's (`probe`, read at `pos`) where it reaches (the sky through
// the canopy and past the ground, the light the ground and the leaves send
// back), else the open sky's.
vec3 sceneAmbientIrradiance(vec3 pos, SceneProbe probe, vec3 n) {
    if (probe.on) return pgShIrradiance(probe.r, probe.g, probe.b, n);
    return sceneOpenSkyIrradiance(pos, n);
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

// Where `pos` falls in map `m`'s mip `mip` (a clipmap level has the one):
// where in its page it lies (texels from the page's first, 0 to SP_PAGE),
// the table words of its page and overlay, its [0,1] depth and its depth
// from the light (1 in an ortho map). False when it lies off the map.
struct ShadowHit {
    vec2 inPage;
    uint page;
    uint overlay;
    float depth;
    float zEye;
};

bool shadowLocate(int m, int mip, vec3 pos, out ShadowHit hit) {
    ShadowMapGpu map = lighting.shadowMaps[m];
    vec4 sc = map.matrix * vec4(pos, 1.0);
    if (sc.w <= 0.0) return false;
    hit.zEye = sc.w;
    sc.xyz /= sc.w;
    bool clipmap = int(map.info.y) == SP_KIND_CLIPMAP;
    int n = clipmap ? SP_WINDOW : (SP_LOCAL >> mip);
    vec2 pc = sc.xy * float(n);
    if (any(lessThan(pc, vec2(0.0))) || any(greaterThanEqual(pc, vec2(float(n))))) return false;
    ivec2 p = min(ivec2(floor(pc)), ivec2(n - 1));
    int e = clipmap ? shadowClipmapEntry(map, p) : shadowLocalEntry(map, mip, p);
    hit.page = shadowTable[2 * e];
    hit.overlay = shadowTable[2 * e + 1];
    hit.inPage = (pc - vec2(p)) * float(SP_PAGE);
    hit.depth = sc.z;
    return true;
}

// A bordered page's shadow at `texel` (its first texel `first`, in a
// texture of 1/`inv` texels a side): 1 lit, 0 shadowed. The filter is a
// grid of hardware-bilinear compares, params.y taps on a side at
// `spread`-texel spacing (3x3 at 1 is a 4-texel tent), kept inside the
// page's bordered square: the border holds what lies past the page's edge,
// drawn from the same casters, so the filter crosses the edge with no seam
// and never reads the texture's next page. One function a texture (the
// pool's pages, the overlays), so neither carries a branch a tap.
#define SCENE_SHADOW_FILTER(NAME, TEX)                                                                       \
    float NAME(vec2 first, vec2 texel, float inv, float ref, float spread) {                                \
        vec2 base = texel * inv;                                                                            \
        vec2 minUV = (first - float(SP_BORDER) + 1.0) * inv;                                                \
        vec2 maxUV = (first + float(SP_PAGE + SP_BORDER) - 1.0) * inv;                                      \
        int hk = (int(lighting.params.y) - 1) / 2;                                                          \
        if (hk <= 0 || spread == 0.0) return texture(TEX, vec3(clamp(base, minUV, maxUV), ref));            \
        if (spread < 0.0) {                                                                                 \
            vec2 o = vec2(inv * (-spread * 0.5 * float(hk)));                                               \
            return 0.25 * (texture(TEX, vec3(clamp(base + vec2(-o.x, -o.y), minUV, maxUV), ref)) +          \
                           texture(TEX, vec3(clamp(base + vec2(o.x, -o.y), minUV, maxUV), ref)) +           \
                           texture(TEX, vec3(clamp(base + vec2(-o.x, o.y), minUV, maxUV), ref)) +           \
                           texture(TEX, vec3(clamp(base + vec2(o.x, o.y), minUV, maxUV), ref)));            \
        }                                                                                                   \
        float s = 0.0;                                                                                      \
        vec2 step = vec2(inv * spread);                                                                     \
        for (int y = -hk; y <= hk; ++y)                                                                     \
            for (int x = -hk; x <= hk; ++x) s += texture(TEX, vec3(clamp(base + vec2(x, y) * step, minUV, maxUV), ref)); \
        float side = float(2 * hk + 1);                                                                     \
        return s / (side * side);                                                                           \
    }
// spread 0: the one hardware-filtered tap; spread < 0: four hardware-
// filtered taps spanning about what the grid at -spread does, at under half
// its cost.
SCENE_SHADOW_FILTER(shadowFilterPage, shadowPool)
SCENE_SHADOW_FILTER(shadowFilterOverlay, shadowOverlay)

// Where map `m`'s mip `mip`, whose texels are `texelW` metres, looks `pos`
// up, and the depth it compares there (`ref`). The lookup point is pushed
// off the surface along the normal and the depth biased by the slope, both
// sized in its texels (about half a texel head-on, more at grazing
// incidence). The caster pass culls light-facing faces and applies a slope
// offset; this is the remainder. False when it lies off the map.
bool shadowProbe(int m, int mip, float texelW, vec3 pos, vec3 N, float NdotL, out ShadowHit hit, out float ref) {
    ShadowMapGpu map = lighting.shadowMaps[m];
    float sinT = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    float nOff = map.bias.y + texelW * (0.5 + sinT);
    float bWorld = texelW * (0.3 + 0.5 * sinT);
    ref = 0.0;
    if (!shadowLocate(m, mip, pos + N * nOff, hit)) return false;
    float span = max(map.depth.y - map.depth.x, 1e-6);
    float b01 = map.depth.z > 0.5 ? bWorld / span
                                  : bWorld * map.depth.x * map.depth.y / (span * hit.zEye * hit.zEye);
    // Past the far plane is behind every caster: compared at it.
    ref = min(hit.depth - map.bias.x - b01, 1.0);
    return true;
}

// A probe's cached page (its word resident) and its overlay of what moves
// this frame (when its overlay word is). The overlay's filter is the four
// hardware-filtered taps over the same span: what it holds sways, and its
// edges are soft anyway.
vec2 shadowPageFirst(ShadowHit hit) {
    // The pool's row and column in float: exact for its few thousand pages,
    // and no integer divide.
    float phys = float(hit.page & SP_PHYSICAL_MASK);
    float row = floor((phys + 0.5) / lighting.params.w);
    return vec2(phys - row * lighting.params.w, row) * float(SP_STRIDE) + float(SP_BORDER);
}
float shadowPageAt(ShadowHit hit, float ref, float spread) {
    vec2 first = shadowPageFirst(hit);
    return shadowFilterPage(first, first + hit.inPage, lighting.params.z, ref, spread);
}
// The share of the four texels about a probe's cached page point holding a
// caster nearer the light than `ref`.
float shadowBlockedAt(ShadowHit hit, float ref) {
    vec2 first = shadowPageFirst(hit);
    float inv = lighting.params.z;
    vec2 uv = clamp((first + hit.inPage) * inv, (first - float(SP_BORDER) + 1.0) * inv,
                    (first + float(SP_PAGE + SP_BORDER) - 1.0) * inv);
    return 1.0 - dot(textureGather(shadowPool, uv, ref), vec4(0.25));
}
float shadowOverlayAt(ShadowHit hit, float ref, float spread) {
    if ((hit.overlay & SP_RESIDENT) == 0u) return 1.0;
    vec2 first = vec2(float(hit.overlay & SP_OVERLAY_MASK), float((hit.overlay >> SP_OVERLAY_BITS) & SP_OVERLAY_MASK));
    return shadowFilterOverlay(first, first + hit.inPage, 1.0 / float(textureSize(shadowOverlay, 0).x), ref,
                               -abs(spread));
}

// A sun's clipmap levels share the light's rotation and depth range and
// differ only in scale (each twice the one before) and where their windows
// lie, so one transform by level 0's matrix (`first`, the same for every
// pixel) serves the walk over levels: a level's (u, v) is level 0's scaled
// by 2^-level plus its own offset, the normal offset sized in its texels
// added along the normal's image.
struct SunProbe {
    vec3 p;       // level 0's (u, v, depth) of the point
    vec3 n;       // and of the unit normal (a direction)
    float sinT;
};

SunProbe sunProbe(int first, vec3 pos, vec3 N, float NdotL) {
    mat4 M = lighting.shadowMaps[first].matrix;
    SunProbe sp;
    sp.p = (M * vec4(pos, 1.0)).xyz;
    sp.n = (M * vec4(N, 0.0)).xyz;
    sp.sinT = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    return sp;
}

// shadowProbe for the sun's level `level`.
bool sunLocate(int first, int level, SunProbe sp, out ShadowHit hit, out float ref) {
    ShadowMapGpu map0 = lighting.shadowMaps[first];
    float scale = exp2(-float(level));
    float texelW = map0.bias.z / scale;
    float nOff = map0.bias.y + texelW * (0.5 + sp.sinT);
    float bWorld = texelW * (0.3 + 0.5 * sp.sinT);
    vec3 q = sp.p + sp.n * nOff;
    ShadowMapGpu map = lighting.shadowMaps[first + level];
    vec2 pc = (q.xy * scale + (map.matrix[3].xy - scale * map0.matrix[3].xy)) * float(SP_WINDOW);
    ref = 0.0;
    if (any(lessThan(pc, vec2(0.0))) || any(greaterThanEqual(pc, vec2(float(SP_WINDOW))))) return false;
    ivec2 p = min(ivec2(floor(pc)), ivec2(SP_WINDOW - 1));
    int e = shadowClipmapEntry(map, p);
    hit.page = shadowTable[2 * e];
    hit.overlay = shadowTable[2 * e + 1];
    hit.inPage = (pc - vec2(p)) * float(SP_PAGE);
    hit.depth = q.z;
    hit.zEye = 1.0;
    ref = min(q.z - map0.bias.x - bWorld / max(map0.depth.y - map0.depth.x, 1e-6), 1.0);
    return true;
}

// Light `i`'s shadow at `pos`. The level (a sun's clipmap) or mip (a spot's
// map, a point light's face) is the one whose texel is about the pixel's
// footprint there, as the marking pass (shadow/shadow_mark.comp) asked for
// it; while its page is not drawn yet the next coarser one that is serves
// (a sun's next finer, when none coarser is).
// The walk reads only table words, so whichever level serves, the page is
// filtered once. What moves over a page this frame is in its overlay, which
// a sun's level finer than the sway level reads from the sway level's.
// A sun's level is filtered as wide in metres as the footprint asks: across
// a level its filter widens by the footprint's share of the way to the next
// (up to twice its texel, the next level's own), so the shadow's softness is
// continuous where the texels double, and a pixel filters one page. A
// shadow cast from far up the light is filtered as wide as the sun's
// penumbra there, where that is wider than the still penumbra: the search
// level (its texel about that penumbra, so its gather looks as far about
// the point as the penumbra reaches; every plant in its pages) is asked
// whether a caster lies as far up as the
// still penumbra's distance, twice it, four times it (a gather of compares
// each), and the share of its texels that do lifts the level toward the
// one whose filter spans the penumbra, so the softening is continuous (a
// point whose footprint is the search level's or coarser is filtered as
// wide already). Its
// shadow fades out over the last tenth of its reach. `spread` widens the
// filter (1: a 4-texel tent at pcf 3; 0: one hardware-filtered tap).
float lightShadow(int i, vec3 pos, float camDist, float lightDist, vec3 N, float NdotL, float spread) {
    SceneLight light = lighting.lights[i];
    int first = int(light.shadow.z);
    if (first < 0) return 1.0;
    int count = int(light.shadow.w);
    float footprint = lighting.shadowView.x > 0.0 ? camDist * lighting.shadowView.x : lighting.shadowView.y;
    ShadowHit hit;
    float ref = 0.0;
    if (int(light.position.w) == 0) {
        float lf = log2(max(footprint / lighting.shadowView.z, 1e-9));
        SunProbe sp = sunProbe(first, pos, N, NdotL);
        int search = int(lighting.shadowSoft.z);
        if (search >= 0 && search < count && lf < float(search) && sunLocate(first, search, sp, hit, ref) &&
            (hit.page & SP_RESIDENT) != 0u) {
            ShadowMapGpu map0 = lighting.shadowMaps[first];
            float still = lighting.shadowSoft.y / (lighting.shadowSoft.x * max(map0.depth.y - map0.depth.x, 1e-6));
            float f0 = shadowBlockedAt(hit, ref - still);
            if (f0 > 0.0) {
                float lp = log2(lighting.shadowSoft.y / (4.0 * lighting.shadowView.z)) +
                           shadowBlockedAt(hit, ref - 2.0 * still) + shadowBlockedAt(hit, ref - 4.0 * still);
                lf = max(lf, mix(lf, lp, f0));
            }
        }
        int want = clamp(int(floor(lf)), 0, count - 1);
        int level = want;
        for (; level < count; ++level)
            if (sunLocate(first, level, sp, hit, ref) && (hit.page & SP_RESIDENT) != 0u) break;
        if (level >= count) {
            // Nothing as coarse is drawn (the first frames over a place): a
            // finer page, the base pages that cover what is seen.
            for (level = want - 1; level >= 0; --level)
                if (sunLocate(first, level, sp, hit, ref) && (hit.page & SP_RESIDENT) != 0u) break;
            if (level < 0) return 1.0;
        }
        float s = shadowPageAt(hit, ref, spread * exp2(clamp(lf - float(level), 0.0, 1.0)));
        int sway = min(int(lighting.shadowView.w), count - 1);
        if (s > 0.0 && sway >= 0) {
            if (level < sway && !sunLocate(first, sway, sp, hit, ref)) hit.overlay = 0u;
            s *= shadowOverlayAt(hit, ref, spread);
        }
        float reach = light.shadowReach.x;
        if (reach > 0.0) s = mix(s, 1.0, smoothstep(0.9 * reach, reach, camDist));
        return s;
    }
    vec3 toFrag = pos - light.position.xyz;
    int m = first + (count == 6 ? shadowCubeFace(toFrag) : 0);
    float texel0 = lighting.shadowMaps[m].bias.w * lightDist;
    int mip = clamp(int(floor(log2(max(footprint / max(texel0, 1e-9), 1e-9)))), 0, SP_LOCAL_MIPS - 1);
    for (; mip < SP_LOCAL_MIPS; ++mip) {
        if (shadowProbe(m, mip, texel0 * exp2(float(mip)), pos, N, NdotL, hit, ref) && (hit.page & SP_RESIDENT) != 0u)
            break;
    }
    if (mip >= SP_LOCAL_MIPS) return 1.0;
    float s = shadowPageAt(hit, ref, spread);
    return s > 0.0 && lighting.shadowView.w >= 0.0 ? s * shadowOverlayAt(hit, ref, spread) : s;
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
    // ground it covers); zero: the shading normal. Shorter than unit (its
    // squared length, at least a quarter), only that share of a sward's
    // light through it comes in swardGlow's lobe, the rest evenly.
    vec3 thinNormal;
    // A sward too fine to resolve (far grass, the turf standing for it):
    // xyz the way its blades stand on average (the ground's normal tipped
    // by the wind's lay), w how much of the direct light is the sward's
    // (sceneSwardLight) rather than the shading normal's (0: none).
    vec4 sward;
};

// The direct light on a sward seen from afar, as a canopy of thin blades
// lit by a distant light: what the eye sees is the blades nearest it along
// its line, and of those the ones the light reaches.
//
//   - The blades it sees: at two tilts from `up` (an upright young leaf, an
//     arching old one) and every way round, each weighted by how much of
//     it the eye sees (the cosine of its face to the eye). A face turned to
//     the light reflects by its cosine to it (Lambert); a face turned away
//     lets through the light falling on its other side, by that cosine
//     (a blade transmits about as diffusely as it reflects). Under a high
//     sun the eye looking across sees blade sides lit at a slant and the
//     undersides of those leaning away: a far meadow is darker than the
//     level ground it covers. Under a low sun behind the eye it sees faces
//     turned to it; looking toward the sun, the shaded sides with the light
//     through them, a dim glow, not a sheet of lit faces.
//   - The share of them in light, as the near blades it stands for show it
//     at the same view: along a low line the eye sees the tops, all lit;
//     from higher it sees deeper in, down to the blades' shaded feet and
//     the ground between them, SWARD_LIT_TOP less the sine of the eye's
//     line to the blades' way. The near blades show no shade of their
//     neighbours that follows the sun (only their height's shade and the
//     shadow map's), so neither does the sward: a turbid canopy's
//     extinction along the sun's line as well (G 0.6 over its cosine each
//     way, with Kuusk's hot spot) left the far meadow 10-16% darker than
//     its near blades at one angle of view, golden and noon alike.
//
// The shadows of what stands over it come from the shadow map as for
// anything else. Returns x the mean cosine of the lit faces the
// eye sees (for the diffuse), y the mean cosine through the faces turned
// away, z the mean of their cuticles' sheen (the specular term, cosine
// included), all times the lit share.
const float SWARD_TILT_A = 0.35;   // rad from upright
const float SWARD_TILT_B = 1.0;
// The ways round each tilt summed, evenly: the wind's lay is all in `up`
// (the near blades' bend, scene_grass.glsl's grassSwardLay). A comb that
// gathered the leans downwind as well, by the lay, made far more of a gust
// than its blades show, and cost the far blades a tenth of a millisecond.
const int SWARD_WAYS = 4;
// The lit share seen straight along the blades' way is SWARD_LIT_TOP - 1
// (meadowtone at 8, 30 and 60 degrees, noon and golden, against 3 m).
const float SWARD_LIT_TOP = 1.25;
// Blinn's exponent for a blade's cuticle (roughness about 0.42 gives 2 /
// r^4 - 2 = 60), widened a little for the curl along a blade.
const float SWARD_SHEEN_POWER = 40.0;   // sceneSwardLight raises to it by squaring (keep in step)
// e^-x I0(x), the modified Bessel function scaled: the mean of
// exp(x (cos - 1)) round a circle, x >= 0. Its ends (1 - x at 0, 1 /
// sqrt(2 pi x) far out) with a rational between them fitted to within 1.9%
// of it over 0..60, smooth and without a branch (Abramowitz & Stegun's two
// polynomials and an exp cost the far blades a tenth of a millisecond).
float swardBesselI0e(float x) {
    const float P = 3.94802, Q = 1.80643, R = 6.34250;
    float x2 = R * x * x;
    return (1.0 + P * x + x2) / (1.0 + Q * x + x2) * inversesqrt(1.0 + 6.2831853 * x);
}
// I1(x) / I0(x), the mean cosine of that lobe (x / 2 small, 1 - 1 / 2x large).
float swardBesselRatio(float x) { return x / (0.5 + sqrt(x * x + 2.25)); }
// sceneSwardLight's frame (below): the eye, the light and the half-way in
// it.
struct SwardFrame { float vu, v1, lu, l1, l2, hu, hr; vec2 hdir; };
// One tilt's blades, SWARD_WAYS ways round (turned `phase` of a step),
// added to sums (sumW, sumD, sumT, sumS); `faces` 2 to take the sheen of
// the faces turned from the eye as well as those turned to it.
void swardTilt(SwardFrame f, float tilt, float phase, int faces, inout vec4 sums) {
    float ca = cos(tilt), sa = sin(tilt);
    float litF = 0.0, litB = 0.0;
    for (int k = 0; k < SWARD_WAYS; ++k) {
        float psi = (float(k) + phase) * (6.2831853 / float(SWARD_WAYS));
        vec2 dir = vec2(cos(psi), sin(psi));
        // A blade leaning `a` toward psi: its face's normal lies in the
        // plane of its lean, square to it (the upper face: up and back).
        // In the frame: n = up sa - (e1 cp + e2 sp) ca.
        float nv = f.vu * sa - f.v1 * dir.x * ca;
        float nl = f.lu * sa - (f.l1 * dir.x + f.l2 * dir.y) * ca;
        // The face the eye sees.
        float w = abs(nv);
        float NL = nv < 0.0 ? -nl : nl;
        float lit = w * max(NL, 0.0);
        sums.xyz += vec3(w, lit, w * max(-NL, 0.0));
        // The lit faces' share for the sheen, front and back faces apart.
        if (nv >= 0.0) litF += lit; else litB += lit;
    }
    // Their cuticles' sheen (a Blinn lobe as wide as a near blade's,
    // roughness about 0.42): where a gust bends the blades, the faces that
    // catch the sun change, and the wave shows whichever way it runs across
    // the view. The lobe is far narrower than the ways a tilt takes, so it
    // is integrated round the blades' ways, not sampled at them (sampled,
    // the half-way swinging past a way as a gust laid the sward drew a thin
    // pale line round every band of the gusts). n.H = A - R cos(psi - psiH):
    // the front faces' lobe peaks at psiH + pi (A + R), the back faces' at
    // psiH (R - A, never over cos(tilt) with the half-way above the sward).
    float A = f.hu * sa, R = ca * f.hr;
    for (int s = 0; s < faces; ++s) {
        float sg = s == 0 ? 1.0 : -1.0;
        float P = R + sg * A;
        // (0.8^40 is 1e-4: under a hundredth of the diffuse light.)
        if (P <= 0.8) continue;
        vec2 dir = -sg * f.hdir;
        float nv = f.vu * sa - f.v1 * dir.x * ca;
        float nl = f.lu * sa - (f.l1 * dir.x + f.l2 * dir.y) * ca;
        // The lit face there, if it is the face the eye sees.
        float atPeak = nv * sg > 0.0 ? abs(nv) * max(nl * sg, 0.0) : 0.0;
        // The lobe round the ways: (n.H)^40 ~ P^40 exp(-x (1 - cos)),
        // x = 40 R / P, whose mean is P^40 e^-x I0(x) and whose first moment
        // I1 / I0 weighs the faces at its peak against the tilt's mean.
        float x = SWARD_SHEEN_POWER * R / P;
        float share = (s == 0 ? litF : litB) / float(SWARD_WAYS);
        float g = share + swardBesselRatio(x) * (atPeak - share);
        float p2 = P * P, p8 = p2 * p2; p8 *= p8;
        float p32 = p8 * p8; p32 *= p32;
        sums.w += float(SWARD_WAYS) * g * p32 * p8 * swardBesselI0e(x);
    }
}
vec3 sceneSwardLight(vec3 up, vec3 V, vec3 L) {
    // A frame about `up`, its first axis toward the eye (the set is
    // symmetric across the view, so the mean is steady as the eye turns).
    vec3 e1 = V - up * dot(V, up);
    float l1 = dot(e1, e1);
    e1 = l1 > 1e-8 ? e1 * inversesqrt(l1) : normalize(cross(up, vec3(0.0, 0.0, 1.0)) + vec3(1e-4, 0.0, 0.0));
    vec3 e2 = cross(up, e1);
    float vu = dot(V, up), v1 = dot(V, e1);
    float lu = dot(L, up), l1L = dot(L, e1), l2 = dot(L, e2);
    vec3 H = normalize(V + L);
    float hu = dot(H, up), h1 = dot(H, e1), h2 = dot(H, e2);
    // Where the wind lays the sward `up` is tipped off the vertical, and a
    // gust running across the view swings the faces the eye sees between
    // those turned to the light and those turned from it.
    SwardFrame f;
    f.vu = vu; f.v1 = v1; f.lu = lu; f.l1 = l1L; f.l2 = l2;
    f.hu = hu; f.hr = sqrt(h1 * h1 + h2 * h2);
    f.hdir = f.hr > 1e-5 ? vec2(h1, h2) / f.hr : vec2(1.0, 0.0);
    vec4 sums = vec4(0.0);   // sumW, sumD, sumT, sumS
    // The second tilt's ways turned half a step; its back faces' sheen peaks
    // at cos(1.0)^40, nothing.
    swardTilt(f, SWARD_TILT_A, 0.125, 2, sums);
    swardTilt(f, SWARD_TILT_B, 0.625, 1, sums);
    float sumW = sums.x, sumD = sums.y, sumT = sums.z, sumS = sums.w;
    // The lit share of the blades seen: over 1 along a low line, the tops
    // it sees lighter than the blades' mean shade the far blade and the
    // turf carry (the world's sward module's).
    float lit = lu > 0.0 ? max(SWARD_LIT_TOP - vu, 0.0) : 0.0;
    // The sheen: normalised Blinn, Schlick's Fresnel from a cuticle's 0.04.
    float vh = max(dot(V, H), 0.0);
    float F = 0.04 + 0.96 * pow(1.0 - vh, 5.0);
    float sheen = sumS * F * (SWARD_SHEEN_POWER + 8.0) / (8.0 * PI);
    return vec3(sumD, sumT, sheen) * (lit / max(sumW, 1e-5));
}
// The colour of the light through a blade over its reflectance's: a thin
// leaf lets through about as much as it reflects in the green (PROSPECT's
// R ~ T), less in the blue, which the chlorophyll takes twice: yellower.
const vec3 SWARD_TRANSMIT = vec3(0.95, 1.0, 0.45);
// How the light through a blade spreads: SWARD_GLOW_SHARE of it in a lobe
// about the sun's own way (cos^SWARD_GLOW_POWER of the angle between the
// eye's line and the sun's, as much light in all as an even spread), the
// rest even (Lambertian). Looking toward a low sun a meadow glows; looking
// across it or down, the blades pass on only their even share.
const float SWARD_GLOW_SHARE = 0.7;
const float SWARD_GLOW_POWER = 4.0;
float swardGlow(vec3 V, vec3 L) {
    float c = max(dot(-V, L), 0.0);
    float c2 = c * c;
    return (1.0 - SWARD_GLOW_SHARE) + SWARD_GLOW_SHARE * (SWARD_GLOW_POWER + 1.0) * c2 * c2;
}

// The direct light of every scene light on `s`, the probe grid read at its
// position `probe` (sceneProbe).
vec3 sceneDirectLight(SceneSurface s, SceneProbe probe) {
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
        // The atmosphere's sun is that light above the air: what reaches the
        // surface is what the air and the planet's shadow leave of it.
        if (SCENE_ATMOSPHERE && i == int(lighting.atm.ground.w)) radiance *= sceneSunTransmittance(s.position);
        // The sun's shade of the land (the land's sun and the probe grid's),
        // over its shadow pages'.
        float land = s.receivesShadow && i == int(lighting.sunDirection.w) - 1 ? sceneLandSun(s.position, probe) : 1.0;
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
            // Four hardware-filtered taps as wide as nine point taps: leaves
            // are most of the frame's lit pixels, and a leaf's shadow is
            // soft (the sun's disc) whichever way it is filtered.
            // (A hill's shade passes on nothing.)
            float vis = land > 0.0 ? lightShadow(i, s.position, s.camDist, lightDist, tN, max(abs(NL), 0.2), -2.0) : 0.0;
            leafShadow = (0.1 + 0.9 * vis) * land;
        }
        // Leaf translucency: light passing through a leaf lit from behind,
        // strongest looking toward the light, a warm yellow-green, and
        // shadowed by the leaves between it and the light.
        // A sward too fine to resolve: its blades' mean (sceneSwardLight),
        // the light through them as diffuse as off them.
        vec3 sward = s.sward.w > 0.0 ? sceneSwardLight(s.sward.xyz, V, L) : vec3(0.0);
        bool thin = dot(s.thinNormal, s.thinNormal) > 0.25;
        float glow = thin ? mix(1.0, swardGlow(V, L), min(dot(s.thinNormal, s.thinNormal), 1.0)) : 1.0;
        vec3 swardThrough = s.baseColor * SWARD_TRANSMIT * (s.sward.w * sward.y * glow / PI);
        if (leafy && s.sward.w < 1.0) {
            if (thin) {
                // A blade near the eye: what comes through its face as the
                // sward's blades pass it on (as much as its face reflects,
                // yellower), so near and far are one meadow; a leaf's art
                // glow, four times this, left the far sward darker than the
                // near by 7-19%.
                float NLt = dot(s.thinNormal, L);
                Lo += s.baseColor * SWARD_TRANSMIT * radiance *
                      (max(-NLt, 0.0) * glow / PI * s.translucency * (1.0 - s.sward.w) * leafShadow);
            } else {
                float through = smoothstep(-0.3, 0.4, 0.3 - NL);
                float back = pow(max(dot(-V, L), 0.0), 3.0);
                float amount = s.translucency * through * (0.1 + 0.9 * back) * (1.0 - s.sward.w);
                vec3 tcol = sqrt(max(s.baseColor, vec3(0.0))) * vec3(0.75, 0.95, 0.35) * 0.55;
                Lo += tcol * radiance * amount * leafShadow;
            }
        }

        // A leaf's diffuse light wraps a little past its edge-on angle, so a
        // leaf turning away from the sun dims rather than switching off.
        float NdotL = max(NL, 0.0);
        float diffNL = leafy ? clamp((NL + 0.3) / 1.3, 0.0, 1.0) : NdotL;
        // A near blade's face the eye sees, where the sun is on it, is lit
        // by its own slope to the sun, not by the softened bulk normal its
        // shading takes (which a low sun barely reaches).
        if (thin) diffNL = max(diffNL, max(dot(s.thinNormal, L), 0.0));
        diffNL = mix(diffNL, sward.x, s.sward.w);
        if (diffNL <= 0.0 && sward.y <= 0.0) continue;
        vec3 H = normalize(V + L);
        float D = distributionGGX(max(dot(N, H), 0.0), s.roughness);
        float G = geometrySmith(NdotV, NdotL, s.roughness);
        vec3 F = fresnelSchlick(max(dot(V, H), 0.0), F0);
        vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
        vec3 diffuse = (vec3(1.0) - F) * (1.0 - s.metallic) * s.baseColor / PI;

        float shadow = leafy ? leafShadow
                     : !s.receivesShadow ? 1.0
                     : land > 0.0 ? land * lightShadow(i, s.position, s.camDist, lightDist, N, max(NdotL, 0.3 * s.sward.w), 1.0)
                     : 0.0;
        vec3 spec = mix(specular * NdotL, vec3(sward.z), s.sward.w);
        Lo += shadow * (diffuse * diffNL + swardThrough + spec) * radiance;
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

// Ambient (indirect) light on `s`, Karis' split sum: with the atmosphere its
// sky light (SH irradiance on the diffuse part, SH radiance on the specular),
// which takes precedence over an environment; with an environment the
// irradiance on the diffuse part and the prefiltered radiance times the BRDF
// LUT on the specular, both turned by the environment's rotation; without
// one the flat ambient on the diffuse part. Where the probe grid reaches, the
// diffuse part is its irradiance, and the specular is dimmed as the probes
// see less than the open sky along R. A bound reflection probe replaces the
// global specular by its weight at the surface (probes are specular-only).
// `probe` is the grid read at the surface's position (sceneProbe).
vec3 sceneAmbient(SceneSurface s, SceneProbe probe) {
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
    vec3 kD = (1.0 - F) * (1.0 - s.metallic);
    // A sward's blades see the sky only over the blades around them: the low
    // sky and the ground beyond are hidden, so its sky light is what comes in
    // at its top, the sky about the way its blades stand, however its faces
    // turn (a far meadow lit by the horizon's glow behind the eye shone a
    // half brighter than the grass at one's feet).
    vec3 nI = s.sward.w > 0.0 ? normalize(mix(s.normal, s.sward.xyz, s.sward.w)) : s.normal;
    vec4 pr = probe.r, pg = probe.g, pb = probe.b;
    bool probed = probe.on;
    if (sceneAtmosphereOn()) {
        // The atmosphere's sky: its light on the surface from every way it
        // faces (the sky above, the lit ground below, through the air at
        // the surface's height and sun), and its smooth radiance along R
        // where no reflection probe covers the surface.
        vec3 c[6];
        mat3 frame;
        atmSkyLightSh(s.position, c, frame);   // once, for both
        vec3 irradiance = atmShIrradiance(c, frame, nI);
        vec3 specular = atmShRadiance(c, frame, R) * (F * brdf.x + brdf.y);
        if (probed) {
            irradiance = pgShIrradiance(pr, pg, pb, nI);
            specular *= sceneSkySeen(pgShIrradiance(pr, pg, pb, R), atmShIrradiance(c, frame, R));
        }
        return kD * irradiance * s.baseColor + mix(specular, probeSpec, probeW);
    }
    if (lighting.iblParams.x > 0.5) {
        float rotation = lighting.iblParams.z;
        float intensity = lighting.iblParams.y;
        vec3 irradiance = sceneOpenSkyIrradiance(s.position, nI);
        vec3 prefiltered = textureLod(texPrefilter, rotateY(R, rotation), s.roughness * lighting.iblParams.w).rgb;
        vec3 specular = prefiltered * (F * brdf.x + brdf.y) * intensity;
        if (probed) {
            irradiance = pgShIrradiance(pr, pg, pb, nI);
            specular *= sceneSkySeen(pgShIrradiance(pr, pg, pb, R), sceneOpenSkyIrradiance(s.position, R));
        }
        return kD * irradiance * s.baseColor + mix(specular, probeSpec, probeW);
    }
    vec3 irradiance = probed ? pgShIrradiance(pr, pg, pb, nI) : lighting.ambientColor.rgb;
    return irradiance * s.baseColor * (1.0 - s.metallic) + probeSpec * probeW;
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

// The air between this view's eye and camera-relative `pos`, from the
// atmosphere's tables. Inside the main camera's frustum: its aerial-
// perspective volume (froxels over the frustum, slices squared in depth,
// a point nearer than the first slice taking the share of it its distance
// is), the lookup dithered by a fraction of a slice so no slice's edge bands.
// Outside it (a reflection probe's faces): the transmittance from the
// transmittance table (the ratio of the two points' to the top) and the
// in-scatter as the sky-view table's radiance along the ray, by the share of
// the ray's air that lies before the point.
void sceneAtmosphereAir(vec3 pos, out vec3 transmittance, out vec3 inscatter) {
    vec3 E = lighting.atm.sunIlluminance.rgb / ATM_TABLE_SCALE;   // both tables read here are scaled
    vec3 p = pos + lighting.atm.froxel.xyz;   // relative to the froxel camera
    vec4 clip = lighting.atm.froxelViewProj * vec4(p, 1.0);
    vec2 ndc = clip.w > 0.0 ? clip.xy / clip.w : vec2(2.0);
    if (all(lessThanEqual(abs(ndc), vec2(1.02)))) {
        float d = length(p);
        float nearWeight;
        float w = atmFroxelW(lighting.atm.froxel.w, d, nearWeight);
        w += (atmNoise(gl_FragCoord.xy) - 0.5) * lighting.atm.skyView.z / ATM_FROXEL_SLICES;
        vec3 uvw = vec3(ndc * 0.5 + 0.5, clamp(w, 0.5 / ATM_FROXEL_SLICES, 1.0 - 0.5 / ATM_FROXEL_SLICES));
        vec3 L = textureLod(texAtmInscatter, uvw, 0.0).rgb;
        vec3 T = textureLod(texAtmAirTransmittance, uvw, 0.0).rgb;
        inscatter = L * nearWeight * E;
        transmittance = mix(vec3(1.0), T, nearWeight);
        return;
    }
    float d = length(pos);
    vec3 ro = atmOrigin(vec3(0.0));
    vec3 rd = pos / max(d, 1e-4);
    vec3 pp = ro + pos;
    float r0 = length(ro), r1 = length(pp);
    float mu0 = dot(ro, rd) / r0, mu1 = dot(pp, rd) / r1;
    bool ground = atmGroundHit(ro, rd) >= 0.0;
    vec3 tFull, tRay;
    if (ground) {
        // Toward the ground: the reversed ray, from the point back up past
        // the eye (a = T(point..eye) * b).
        vec3 a = atmTransmittanceToTop(r1, -mu1);
        vec3 b = atmTransmittanceToTop(r0, -mu0);
        tRay = clamp(a / max(b, vec3(1e-6)), 0.0, 1.0);
        tFull = vec3(0.0);
    } else {
        vec3 a = atmTransmittanceToTop(r0, mu0);
        vec3 b = atmTransmittanceToTop(r1, mu1);
        tRay = clamp(a / max(b, vec3(1e-6)), 0.0, 1.0);
        tFull = a;
    }
    vec3 up = ro / r0, xAxis, yAxis;
    float muS;
    atmSunFrame(up, xAxis, yAxis, muS);
    vec2 h = vec2(dot(rd, xAxis), dot(rd, yAxis));
    float hl = length(h);
    float cosAz = hl > 1e-5 ? h.x / hl : 1.0;
    vec3 sky = textureLod(texAtmSkyView, atmSkyViewUv(lighting.atm.skyView.x, dot(rd, up), cosAz), 0.0).rgb;
    vec3 share = (1.0 - tRay) / max(1.0 - tFull, vec3(1e-4));
    transmittance = tRay;
    inscatter = sky * clamp(share, 0.0, 1.0) * E;
}

SceneAir sceneAir(vec3 pos, float camDist) {
    SceneAir air;
    air.transmittance = vec3(1.0);
    air.inscatter = vec3(0.0);
    air.fade = 0.0;
    if (sceneAtmosphereOn()) {
        sceneAtmosphereAir(pos, air.transmittance, air.inscatter);
        return air;
    }
    float fog = fogFactorFor(camDist, pos.y + camera.eyeWorld.y);
    air.transmittance = vec3(1.0 - fog);
    air.inscatter = camera.fogColor.rgb * fog;
    air.fade = fog;
    return air;
}

// The tile shade map's value under `pos` (see scene/world/shade_map.h): square
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
