// Set 1 of every lit scene pipeline and the shading every lit surface shares:
// Cook-Torrance PBR over the frame's lights, the shadow atlas, the flat
// ambient with the bound reflection probe's specular, fog and the tile shade
// map. Mirrors SceneLightingUniforms (scene_vk_descriptors.h); filled by
// scene_lighting.cpp from SceneRenderer's light list and shadow plan.
// Needs scene_camera.glsl included first.
//
//   D = GGX (Trowbridge-Reitz), F = Schlick, G = Smith (Schlick-GGX).
// Light types: 0 = directional, 1 = point, 2 = spot. Range uses the
// Epic/Frostbite smooth window, pow(saturate(1 - (d/range)^4), 2) / (d^2 + 1);
// a spot fades smoothly from its inner to its outer cone.

const float PI = 3.14159265359;

#define SCENE_MAX_LIGHTS 32
#define SCENE_MAX_SHADOW_TILES 16

struct SceneLight {
    vec4 position;      // xyz world position, w type
    vec4 direction;     // xyz unit direction (light -> scene), w range (0 = unbounded)
    vec4 color;         // rgb linear, a intensity
    vec4 shadow;        // x cos(inner), y cos(outer), z first atlas tile (-1 = none), w tile count
    vec4 cascadeSplit;  // far view distance of each cascade (directional CSM)
};

// One atlas tile. `matrix` takes a world position straight to (tile uv, [0,1]
// depth); `rect` places the tile in the atlas. Biases are in world units and
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
} lighting;

layout(set = 1, binding = 1) uniform sampler2DShadow shadowAtlas;
layout(set = 1, binding = 2) uniform samplerCube texReflectionProbe;
layout(set = 1, binding = 3) uniform sampler2D texShadeMap;

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
// one-texel spacing (3x3 is a 4-texel tent), clamped inside the tile so a
// neighbour never bleeds in.
float sampleShadow(int slot, vec3 pos, float biasWorld) {
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
    if (hk <= 0) return texture(shadowAtlas, vec3(clamp(base, minUV, maxUV), ref));
    float s = 0.0;
    for (int y = -hk; y <= hk; ++y) {
        for (int x = -hk; x <= hk; ++x) {
            s += texture(shadowAtlas, vec3(clamp(base + vec2(x, y) * texel, minUV, maxUV), ref));
        }
    }
    float side = float(2 * hk + 1);
    return s / (side * side);
}

// The shadow term of tile `slot`: the lookup point pushed off the surface
// along the normal and a slope-scaled depth bias, both sized in shadow texels
// (about half a texel head-on, more at grazing incidence). The caster pass
// culls light-facing faces and applies a slope offset; this is the remainder.
float shadowFromTile(int slot, vec3 pos, float lightDist, vec3 N, float NdotL) {
    ShadowTile tile = lighting.shadows[slot];
    float texelW = tile.bias.z + tile.bias.w * lightDist;
    float sinT = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    float nOff = tile.bias.y + texelW * (0.5 + sinT);
    float bWorld = texelW * (0.3 + 0.5 * sinT);
    return sampleShadow(slot, pos + N * nOff, bWorld);
}

// Light `i`'s shadow at `pos`: the cube face of a point light, the cascade of
// a directional one (blended into the next across the last 15% of each), or
// the single tile of anything else.
float lightShadow(int i, vec3 pos, float camDist, float lightDist, vec3 N, float NdotL) {
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
        return shadowFromTile(slot + face, pos, lightDist, N, NdotL);
    }
    if (type == 0 && count > 1) {
        vec4 splits = light.cascadeSplit;
        int c = 0;
        if (count >= 2 && camDist > splits.x) c = 1;
        if (count >= 3 && camDist > splits.y) c = 2;
        if (count >= 4 && camDist > splits.z) c = 3;
        float thisFar = splits[c];
        float prevFar = c == 0 ? 0.0 : splits[c - 1];
        float shadow = shadowFromTile(slot + c, pos, lightDist, N, NdotL);
        if (c < count - 1) {
            float blendStart = thisFar - max(thisFar - prevFar, 1e-4) * 0.15;
            float tb = clamp((camDist - blendStart) / max(thisFar - blendStart, 1e-4), 0.0, 1.0);
            if (tb > 0.0) shadow = mix(shadow, shadowFromTile(slot + c + 1, pos, lightDist, N, NdotL), tb);
        }
        return shadow;
    }
    return shadowFromTile(slot, pos, lightDist, N, NdotL);
}

// A lit surface, as every lit shader hands it to sceneDirectLight/sceneAmbient.
struct SceneSurface {
    vec3 position;     // world
    vec3 normal;       // unit shading normal
    vec3 view;         // unit, surface -> eye
    float camDist;
    vec3 baseColor;
    float metallic;
    float roughness;   // clamped to [0.04, 1]
    bool receivesShadow;
    bool twoSided;
    float subsurface;  // wrap-light translucency, two-sided surfaces only
};

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
        if (s.twoSided && s.subsurface > 0.0) {
            float wrap = max(0.0, dot(-N, L) + s.subsurface) * s.subsurface;
            Lo += s.baseColor * radiance * wrap * 0.6;
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;
        vec3 H = normalize(V + L);
        float D = distributionGGX(max(dot(N, H), 0.0), s.roughness);
        float G = geometrySmith(NdotV, NdotL, s.roughness);
        vec3 F = fresnelSchlick(max(dot(V, H), 0.0), F0);
        vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
        vec3 diffuse = (vec3(1.0) - F) * (1.0 - s.metallic) * s.baseColor / PI;

        float shadow = s.receivesShadow ? lightShadow(i, s.position, s.camDist, lightDist, N, NdotL) : 1.0;
        Lo += shadow * (diffuse + specular) * radiance * NdotL;
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

// Ambient (indirect) light on `s`: the flat ambient on the diffuse part plus
// the reflection probe's specular where one is bound.
vec3 sceneAmbient(SceneSurface s) {
    vec3 ambient = lighting.ambientColor.rgb * s.baseColor * (1.0 - s.metallic);
    if (lighting.probePos.w > 0.5) {
        float w = 0.0;
        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
        vec3 F = fresnelSchlickRoughness(max(dot(s.normal, s.view), 1e-4), F0, s.roughness);
        vec3 raw = probeRadiance(s.position, reflect(-s.view, s.normal), s.roughness, w);
        ambient += raw * F * lighting.probeParams.x * w;
    }
    return ambient;
}

// Fog in [0,1] for a fragment `camDist` from the eye at height `worldY`:
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
