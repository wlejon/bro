// Fragment shader: Cook-Torrance PBR, forward multi-light.
//   D = GGX (Trowbridge-Reitz), F = Schlick, G = Smith height-correlated.
// All positions/directions are camera-relative; uCameraPos is implicitly
// the origin, so V = normalize(-vWorldPos).
//
// Light array uniforms are parallel arrays (not a struct) so drivers with
// poor struct-array layout don't punt them to slow paths. Types:
//   0 = directional, 1 = point, 2 = spot.
// Range uses the Epic/Frostbite smooth window:
//   win = pow(saturate(1 - (d/range)^4), 2) / (d^2 + 1)
// Spot cone:
//   cos falloff between outerCos (=0) and innerCos (=1), smoothstep.

#version 330 core
#define MAX_LIGHTS 32
#define MAX_SHADOWS 16

in vec3 vWorldPos;   // camera-relative
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vCamDist;
in vec3 vTangentW;
in vec3 vBitangentW;

uniform vec4 uColor;           // baseColor RGBA (alpha = mesh transparency)
uniform float uEmissive;       // scalar emissive multiplier (0 = off)
uniform vec3 uEmissiveColor;   // per-mesh emissive tint (linear RGB)
uniform float uMetallic;
uniform float uRoughness;
uniform int uUseVertexColor;
uniform int uUseTexture;
uniform sampler2D uBaseColorTex;

// Extended PBR maps. uHas*Map gates sampling so meshes without these maps
// keep their previous (scalar-only) appearance.
uniform int       uHasTangent;
uniform int       uHasNormalMap;
uniform int       uHasMRMap;
uniform int       uHasAOMap;
uniform int       uHasEmissiveMap;
uniform sampler2D uNormalMap;
uniform sampler2D uMRMap;        // glTF: G=roughness, B=metallic
uniform sampler2D uAOMap;        // R channel
uniform sampler2D uEmissiveMap;  // RGB, multiplied by uEmissive * uEmissiveColor
uniform int       uReceivesShadow;

uniform float uFogStart;
uniform float uFogEnd;
uniform vec3 uFogColor;
// Exponential-squared + height fog (setFog {density, heightFalloff,
// startDistance}). uFogDensity > 0 selects this mode over the legacy
// linear ramp above. vWorldPos is camera-relative, so world height is
// reconstructed as vWorldPos.y + uFogCamY.
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogStartDist;
uniform float uFogCamY;
uniform float uAlphaCutoff;     // 0 = no cutoff, >0 discards baseAlpha < cutoff
uniform float uNearClip;

uniform vec3 uAmbient;         // flat ambient fallback used when IBL is disabled
uniform int uUnlit;            // 1 = skip lighting, output baseColor + emissive
uniform int uSSRMask;          // 1 = SSR mask phase: alpha carries the SSR
                               // reflectance mask instead of coverage (opaque
                               // passes only; the SSR pass restores coverage)
uniform int uTwoSided;         // 1 = backface culling disabled at the host
uniform float uSubsurface;     // 0..1; >0 enables wrap-light leaf translucency

uniform int uLightCount;
uniform int   uLightType[MAX_LIGHTS];
uniform vec3  uLightPos[MAX_LIGHTS];      // camera-relative world position
uniform vec3  uLightDir[MAX_LIGHTS];      // unit direction (dir/spot)
uniform vec3  uLightColor[MAX_LIGHTS];    // linear RGB
uniform float uLightIntensity[MAX_LIGHTS];
uniform float uLightRange[MAX_LIGHTS];
uniform vec2  uLightSpotCos[MAX_LIGHTS];  // .x = cos(innerAngle), .y = cos(outerAngle)
uniform int   uLightShadowSlot[MAX_LIGHTS];      // -1 if unshadowed, else 0..MAX_SHADOWS-1
uniform int   uLightShadowSlotCount[MAX_LIGHTS]; // 1 normally; 2..4 for directional CSM
uniform vec4  uLightCascadeSplit[MAX_LIGHTS];    // far view-distance per cascade (CSM only)

// Atlas-tiled shadow maps. One sampler regardless of light count.
//   uShadowMatrix: bias * lightProj * lightView * translate(cameraEye), so
//     `uShadowMatrix[s] * vec4(vWorldPos, 1)` directly produces UV+depth in [0,1].
//   uShadowAtlasRect: (origin.xy, size.xy) of the slot's tile in atlas UV.
//   uShadowBias: (constant depth bias, normal-offset world units).
uniform sampler2DShadow uShadowAtlas;
uniform mat4  uShadowMatrix[MAX_SHADOWS];
uniform vec4  uShadowAtlasRect[MAX_SHADOWS];
uniform vec2  uShadowBias[MAX_SHADOWS];
uniform float uShadowAtlasTexel;          // 1.0 / atlasSize, for PCF kernel
uniform int   uShadowPCFTaps;             // 1 (single sample) or 3 (3x3 PCF)

// IBL — when uIBLEnabled == 1, replaces uAmbient with split-sum env lighting.
//   uIBLIrradiance: pre-convolved cosine hemisphere → diffuse term.
//   uIBLPrefilter:  GGX prefilter mip chain → specular term, sampled at LOD =
//                   roughness * uIBLPrefilterMaxLOD.
//   uIBLBRDF:       2D LUT(NdotV, roughness) → (Fr scale, Fr bias).
uniform int         uIBLEnabled;
uniform samplerCube uIBLIrradiance;
uniform samplerCube uIBLPrefilter;
uniform sampler2D   uIBLBRDF;
uniform float       uIBLIntensity;
uniform float       uIBLRotation;
uniform float       uIBLPrefilterMaxLOD;

// Local reflection probe — when uProbeEnabled == 1, this draw's SPECULAR
// ambient samples the probe's GGX-prefiltered capture instead of the global
// uIBLPrefilter, box-projected (parallax-corrected against the probe's box
// volume) when uProbeBoxProjection == 1, fading back to the global specular
// over uProbeBlendDist world units near the box faces. Diffuse ambient stays
// global (irradiance or flat uAmbient) — probes are specular-only. Probe
// captures are world-axis-aligned (no uIBLRotation analog). One probe per
// draw, selected CPU-side (see scene_renderer_probes.cpp).
uniform int         uProbeEnabled;
uniform samplerCube uProbeSpecular;
uniform mat4        uProbeWorldToLocal;  // camera-relative world -> unit-box space
uniform mat4        uProbeLocalToWorld;  // unit-box space -> camera-relative world
uniform vec3        uProbePos;           // capture origin, camera-relative
uniform vec3        uProbeBoxSize;       // world-space box size per probe axis
uniform int         uProbeBoxProjection;
uniform float       uProbeIntensity;
uniform float       uProbeBlendDist;     // interior fade margin (world units; 0 = hard edge)
uniform float       uProbeMaxLOD;

out vec4 FragColor;

const float PI = 3.14159265359;

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
    return geometrySchlickGGX(NdotV, roughness)
         * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    float f = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (1.0 - F0) * f;
}

// Sebastien Lagarde's Fresnel-with-roughness adaptation. Used for the IBL
// ambient term where there's no half-vector — rough surfaces should get
// bigger F at grazing angles, not the same F a mirror would.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    float f = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * f;
}

// Rotate a direction around the +Y axis. Same convention as the skybox
// pass so the visible sky and the IBL-sampled environment stay aligned.
vec3 rotateY(vec3 d, float a) {
    float c = cos(a), s = sin(a);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

// Local-probe specular radiance along the reflection vector R (camera-
// relative world space). Writes the probe weight to `w`: 1 inside the box
// beyond the interior margin, ramping to 0 at the box faces (fragments
// outside the box — possible since selection is per-mesh, not per-fragment —
// get 0 and fall back to the global environment). Box projection is the
// standard parallax correction: intersect the reflection ray with the box in
// probe space (slab test against the unit box) and sample the direction from
// the capture origin to the hit point.
vec3 probeRadiance(vec3 R, float rough, out float w) {
    vec3 lp = (uProbeWorldToLocal * vec4(vWorldPos, 1.0)).xyz;
    vec3 edgeDist = (vec3(0.5) - abs(lp)) * uProbeBoxSize;  // world units
    float dmin = min(min(edgeDist.x, edgeDist.y), edgeDist.z);
    w = (uProbeBlendDist > 0.0) ? clamp(dmin / uProbeBlendDist, 0.0, 1.0)
                                : step(0.0, dmin);
    vec3 dir = R;
    if (uProbeBoxProjection == 1) {
        vec3 ld = mat3(uProbeWorldToLocal) * R;
        // Nudge components off exact zero so the slab test can't hit 0 * inf.
        ld = mix(ld, vec3(1e-6), vec3(lessThan(abs(ld), vec3(1e-6))));
        vec3 invD = 1.0 / ld;
        vec3 t1 = (vec3(-0.5) - lp) * invD;
        vec3 t2 = (vec3( 0.5) - lp) * invD;
        vec3 tm = max(t1, t2);
        float tHit = min(min(tm.x, tm.y), tm.z);
        vec3 hit = (uProbeLocalToWorld * vec4(lp + ld * tHit, 1.0)).xyz;
        vec3 d2 = hit - uProbePos;
        if (dot(d2, d2) > 1e-8) dir = d2;
    }
    return textureLod(uProbeSpecular, dir, rough * uProbeMaxLOD).rgb;
}

// Smooth distance window (Epic/Frostbite). Vanishes past `range`.
float distanceAttenuation(float dist, float range) {
    if (range <= 0.0) return 1.0;
    float t = dist / range;
    float t4 = t * t * t * t;
    float win = clamp(1.0 - t4, 0.0, 1.0);
    win = win * win;
    return win / (dist * dist + 1.0);
}

// Rotated Poisson-disk taps for PCF. Pre-unit-scaled; caller multiplies by
// (texel * radius) and rotates per-fragment. 16 taps give enough coverage
// to hide individual shadow-map texel edges when the light projects the
// atlas at a grazing angle (long sun shadows on a near-horizontal plane).
const vec2 kPoisson16[16] = vec2[16](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

// Sample one tile of the shadow atlas. Returns 1.0 = lit, 0.0 = shadowed.
// `posCamRel` is the camera-relative world position to test (already normal-
// biased by the caller). `slot` is the per-light shadow slot 0..MAX_SHADOWS-1.
// Out-of-frustum points return 1.0 (no shadow). PCF kernel stays inside the
// tile via inset clamping so neighbouring tiles don't bleed in.
float sampleShadow(int slot, vec3 posCamRel) {
    vec4 sc = uShadowMatrix[slot] * vec4(posCamRel, 1.0);
    if (sc.w <= 0.0) return 1.0;
    sc /= sc.w;
    if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0)
        return 1.0;
    float ref = sc.z - uShadowBias[slot].x;
    vec2  rect_o = uShadowAtlasRect[slot].xy;
    vec2  rect_s = uShadowAtlasRect[slot].zw;
    vec2  texel  = vec2(uShadowAtlasTexel);
    vec2  base   = rect_o + sc.xy * rect_s;
    // Inset by the PCF radius so rotated taps can't straddle into a
    // neighbouring tile (which would sample unrelated depths).
    float radius = 2.0;
    vec2  minUV  = rect_o + texel * (radius + 0.5);
    vec2  maxUV  = rect_o + rect_s - texel * (radius + 0.5);
    if (uShadowPCFTaps <= 1) {
        vec2 uv = clamp(base, minUV, maxUV);
        return texture(uShadowAtlas, vec3(uv, ref));
    }
    // Per-fragment rotation randomises the kernel so nearby fragments sample
    // in different directions — dithers away banded Mach lines at shadow
    // edges and shadow-map texel stretch at grazing light angles.
    float ang = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float cs = cos(ang), sn = sin(ang);
    mat2 rot = mat2(cs, -sn, sn, cs);
    float s = 0.0;
    for (int k = 0; k < 16; ++k) {
        vec2 off = rot * kPoisson16[k] * texel * radius;
        vec2 uv  = clamp(base + off, minUV, maxUV);
        s += texture(uShadowAtlas, vec3(uv, ref));
    }
    return s * (1.0 / 16.0);
}

// Fog factor in [0,1] for a fragment `camDist` from the eye at world height
// `worldY`. Two modes: exponential-squared height fog when uFogDensity > 0
// (density decays with height when uFogHeightFalloff > 0, no fog closer than
// uFogStartDist), else the legacy linear start/end ramp when uFogEnd > 0.
// Returns 0 when fog is fully off.
float fogFactorFor(float camDist, float worldY) {
    if (uFogDensity > 0.0) {
        float d = max(camDist - uFogStartDist, 0.0);
        float dens = uFogDensity;
        if (uFogHeightFalloff > 0.0) dens *= exp(-uFogHeightFalloff * worldY);
        float x = dens * d;
        return 1.0 - exp(-x * x);
    }
    if (uFogEnd > 0.0) {
        float f = clamp((camDist - uFogStart) / (uFogEnd - uFogStart), 0.0, 1.0);
        return f * f;
    }
    return 0.0;
}

// Custom-shader splice point. When a mesh has a user shader, the renderer
// replaces this marker line with the user's GLSL chunk (which must define
// `void userFragment(inout vec3 baseColor, inout vec3 normal,
//                    inout float metallic, inout float roughness,
//                    inout vec3 emissive, inout float alpha)`)
// and injects `#define CUSTOM_FRAGMENT 1` after the #version line. With no
// user chunk the marker is an inert comment and the source is unchanged.
//__USER_CHUNK__

void main() {
    if (uNearClip > 0.0 && vCamDist < uNearClip) discard;

    vec3 baseColor;
    float baseAlpha;
    if (uUseTexture == 1) {
        // Texture composes with the per-vertex tint (vColor is forced to white
        // when the mesh has no vertex colors, so this is a no-op there). Lets a
        // textured mesh still carry per-vertex shading — e.g. tile AO.
        vec4 tex = texture(uBaseColorTex, vUV);
        baseColor = tex.rgb * uColor.rgb * vColor.rgb;
        baseAlpha = tex.a   * uColor.a   * vColor.a;
    } else if (uUseVertexColor == 1) {
        baseColor = vColor.rgb;
        baseAlpha = vColor.a;
    } else {
        baseColor = uColor.rgb;
        baseAlpha = uColor.a;
    }

    if (uAlphaCutoff > 0.0 && baseAlpha < uAlphaCutoff) discard;

    if (uUnlit == 1) {
        vec3 color = baseColor;
        float fogFactorU = fogFactorFor(vCamDist, vWorldPos.y + uFogCamY);
        if (fogFactorU > 0.0) {
            color = mix(color, uFogColor, fogFactorU);
            baseAlpha = mix(baseAlpha, 0.0, fogFactorU);
        }
        // SSR mask phase: unlit surfaces don't reflect (mask 0). Coverage
        // is restored by the SSR pass right after the opaque passes.
        FragColor = vec4(color, (uSSRMask == 1) ? 0.0 : baseAlpha);
        return;
    }

    vec3 N = normalize(vNormal);
    // Two-sided thin surfaces (leaves, petals, fabric) expose their back face
    // whenever the card faces away from the camera. The card bakes a single
    // geometric normal (its +Y), so without this flip a back-facing leaf is lit
    // by a normal pointing away from the viewer — randomly-oriented scattered
    // foliage then lights up at random. Flip the shading normal to the side we
    // actually see so lighting responds to the visible face.
    if (uTwoSided == 1 && !gl_FrontFacing) N = -N;
    if (uHasTangent == 1 && uHasNormalMap == 1) {
        vec3 nTS = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
        mat3 TBN = mat3(normalize(vTangentW), normalize(vBitangentW), N);
        N = normalize(TBN * nTS);
    }
    vec3 V = normalize(-vWorldPos);              // eye at origin (cam-relative)

    // Material params — start from scalars, optionally modulated by MR map.
    float metal = uMetallic;
    float rough = uRoughness;
    if (uHasMRMap == 1) {
        vec4 mr = texture(uMRMap, vUV);
        rough *= mr.g;
        metal *= mr.b;
    }

    vec3 emissive = uEmissiveColor * uEmissive;
    if (uHasEmissiveMap == 1) {
        emissive *= texture(uEmissiveMap, vUV).rgb;
    }

#ifdef CUSTOM_FRAGMENT
    // Custom-shader hook: runs after every material input (base color +
    // texture, MR map, normal map, emissive map) is gathered and before the
    // light loop, so any of the six values can be rewritten and standard PBR
    // lighting applies to the result. `normal` is world-space; it is
    // renormalized (and roughness re-clamped) below.
    userFragment(baseColor, N, metal, rough, emissive, baseAlpha);
    N = normalize(N);
#endif

    float NdotV = max(dot(N, V), 1e-4);
    rough = clamp(rough, 0.04, 1.0);  // floor to avoid spec singularity
    vec3 F0 = mix(vec3(0.04), baseColor, metal);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < uLightCount && i < MAX_LIGHTS; ++i) {
        int t = uLightType[i];
        vec3 L;
        float atten = 1.0;

        if (t == 0) {
            // Directional: uLightDir points FROM light TO scene; invert for L.
            L = normalize(-uLightDir[i]);
        } else {
            vec3 toLight = uLightPos[i] - vWorldPos;
            float d = length(toLight);
            if (d < 1e-4) continue;
            L = toLight / d;
            atten = distanceAttenuation(d, uLightRange[i]);
            if (t == 2) {
                // Spot cone falloff.
                vec3 spotDir = normalize(-uLightDir[i]);  // points AT light
                float c = dot(-L, uLightDir[i] / max(length(uLightDir[i]), 1e-4));
                float innerC = uLightSpotCos[i].x;
                float outerC = uLightSpotCos[i].y;
                float coneT = clamp((c - outerC) / max(innerC - outerC, 1e-4), 0.0, 1.0);
                atten *= coneT * coneT * (3.0 - 2.0 * coneT);
            }
            if (atten <= 0.0) continue;
        }

        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D = distributionGGX(NdotH, rough);
        float G = geometrySmith(NdotV, NdotL, rough);
        vec3  F = fresnelSchlick(VdotH, F0);

        vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metal);
        vec3 diffuse = kD * baseColor / PI;

        float shadow = 1.0;
        int slot = uLightShadowSlot[i];
        if (slot >= 0) {
            int sc = uLightShadowSlotCount[i];
            // Slope-scaled normal offset: grazing surfaces (NdotL near 0) are
            // where constant bias falls down and acne shows up. Push harder
            // there, stay tight where the surface faces the light.
            float slopeK = clamp(1.0 - NdotL, 0.0, 1.0);
            float biasScale = 1.0 + slopeK * 4.0;
            if (t == 1 && sc == 6) {
                // Point light cube unfolded to 6 atlas tiles. Slot order
                // follows the standard cube-map face convention:
                //   0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
                vec3 toFrag = vWorldPos - uLightPos[i];
                vec3 absL = abs(toFrag);
                int face;
                if (absL.x >= absL.y && absL.x >= absL.z)
                    face = (toFrag.x > 0.0) ? 0 : 1;
                else if (absL.y >= absL.z)
                    face = (toFrag.y > 0.0) ? 2 : 3;
                else
                    face = (toFrag.z > 0.0) ? 4 : 5;
                int chosen = slot + face;
                vec3 posBiased = vWorldPos + N * uShadowBias[chosen].y * biasScale;
                shadow = sampleShadow(chosen, posBiased);
            } else if (t == 0 && sc > 1) {
                // Directional CSM: pick the tightest cascade containing this
                // fragment by view-space distance. Splits[] are padded with a
                // sentinel so unrolled compares work for 1..4 slots.
                vec4 splits = uLightCascadeSplit[i];
                int c = 0;
                if (sc >= 2 && vCamDist > splits.x) c = 1;
                if (sc >= 3 && vCamDist > splits.y) c = 2;
                if (sc >= 4 && vCamDist > splits.z) c = 3;
                float thisFar, prevFar;
                if      (c == 0) { thisFar = splits.x; prevFar = 0.0;       }
                else if (c == 1) { thisFar = splits.y; prevFar = splits.x;  }
                else if (c == 2) { thisFar = splits.z; prevFar = splits.y;  }
                else             { thisFar = splits.w; prevFar = splits.z;  }
                int chosen = slot + c;
                vec3 posBiased = vWorldPos + N * uShadowBias[chosen].y * biasScale;
                shadow = sampleShadow(chosen, posBiased);
                // Fade-blend into the next cascade across the last 15% of
                // this one so the resolution hand-off doesn't leave a seam.
                if (c < sc - 1) {
                    float range = max(thisFar - prevFar, 1e-4);
                    float blendStart = thisFar - range * 0.15;
                    float tb = clamp((vCamDist - blendStart)
                                   / max(thisFar - blendStart, 1e-4), 0.0, 1.0);
                    if (tb > 0.0) {
                        int nxt = slot + c + 1;
                        vec3 pb2 = vWorldPos + N * uShadowBias[nxt].y * biasScale;
                        float s2 = sampleShadow(nxt, pb2);
                        shadow = mix(shadow, s2, tb);
                    }
                }
            } else {
                // Single-tile: spot, single-cascade directional, etc.
                vec3 posBiased = vWorldPos + N * uShadowBias[slot].y * biasScale;
                shadow = sampleShadow(slot, posBiased);
            }
            if (uReceivesShadow == 0) shadow = 1.0;
        }

        vec3 radiance = uLightColor[i] * uLightIntensity[i] * atten;
        Lo += shadow * (diffuse + specular) * radiance * NdotL;

        // Cheap wrap-light approximation for thin two-sided surfaces (leaves):
        // adds back-side illumination relative to L. Gated on twoSided + subsurface.
        if (uTwoSided == 1 && uSubsurface > 0.0) {
            float wrap = max(0.0, dot(-N, L) + uSubsurface) * uSubsurface;
            Lo += baseColor * radiance * wrap * 0.6;
        }
    }

    vec3 ambient;
    {
        // Local probe specular (if a probe is bound to this draw). Computed
        // first so both the IBL and flat-ambient paths can blend it in;
        // probeW = 0 leaves them untouched.
        float probeW = 0.0;
        vec3 probeSpec = vec3(0.0);
        if (uProbeEnabled == 1) {
            vec3 R_p    = reflect(-V, N);
            vec3 F_p    = fresnelSchlickRoughness(NdotV, F0, rough);
            vec2 brdfP  = texture(uIBLBRDF, vec2(NdotV, rough)).rg;
            vec3 raw    = probeRadiance(R_p, rough, probeW);
            probeSpec   = raw * (F_p * brdfP.x + brdfP.y) * uProbeIntensity;
        }
        if (uIBLEnabled == 1) {
            // Karis 2013 split-sum IBL. Sample the diffuse irradiance and the
            // GGX-prefiltered specular along the reflection vector, multiplied
            // by the env-independent BRDF LUT. The rotateY calls keep the
            // sampled environment aligned with the skybox under uIBLRotation.
            vec3 R    = reflect(-V, N);
            vec3 N_s  = rotateY(N, uIBLRotation);
            vec3 R_s  = rotateY(R, uIBLRotation);

            vec3 F  = fresnelSchlickRoughness(NdotV, F0, rough);
            vec3 kS = F;
            vec3 kD = (1.0 - kS) * (1.0 - metal);

            vec3 irradiance = texture(uIBLIrradiance, N_s).rgb;
            vec3 diffuse    = irradiance * baseColor;

            float lod = rough * uIBLPrefilterMaxLOD;
            vec3 prefiltered = textureLod(uIBLPrefilter, R_s, lod).rgb;
            vec2 brdf        = texture(uIBLBRDF, vec2(NdotV, rough)).rg;
            vec3 specular    = prefiltered * (F * brdf.x + brdf.y);

            // Probe replaces the GLOBAL specular by its interior weight;
            // diffuse irradiance stays global (probes are specular-only).
            // Each term keeps its own intensity control.
            ambient = kD * diffuse * uIBLIntensity
                    + mix(specular * uIBLIntensity, probeSpec, probeW);
        } else {
            // Fallback for scenes with no environment loaded — flat tint,
            // plus the probe specular where one is bound.
            ambient = uAmbient * baseColor * (1.0 - metal) + probeSpec * probeW;
        }
    }
    if (uHasAOMap == 1) {
        ambient *= texture(uAOMap, vUV).r;
    }
    vec3 color = Lo + ambient + emissive;

    // SSR mask phase (opaque passes while scene SSR is enabled): repurpose
    // the alpha channel to carry the reflectance mask the SSR pass weights
    // reflections by — luminance of F0 (normal-incidence Fresnel, i.e. 0.04
    // for dielectrics up to baseColor luminance for metals) scaled by
    // perceptual smoothness^2, so rough surfaces reflect little and mirrors
    // reflect fully. The fog fade below applies to the mask too, fading
    // reflections out with the surface. The SSR pass consumes the mask and
    // restores alpha to coverage before any pass blends against dest alpha.
    if (uSSRMask == 1) {
        baseAlpha = dot(F0, vec3(0.2126, 0.7152, 0.0722))
                  * (1.0 - rough) * (1.0 - rough);
    }

    // Fog (applied in linear space; tonemap runs after). Exponential-squared
    // height fog or the legacy linear ramp — see fogFactorFor.
    float fogFactor = fogFactorFor(vCamDist, vWorldPos.y + uFogCamY);
    if (fogFactor > 0.0) {
        color = mix(color, uFogColor, fogFactor);
        baseAlpha = mix(baseAlpha, 0.0, fogFactor);
    }

    FragColor = vec4(color, baseAlpha);
}
