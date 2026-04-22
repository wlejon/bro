#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"
#include "physics/physics_world.h"
#include "render/skia_backend.h"
#include "layout/font_manager.h"
#include "dom/document.h"
#include "util/log.h"
#include "brogameagent/world.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>

namespace bro::scene {

// ---------------------------------------------------------------------------
// Mesh shader sources (GLSL 330 core)
// ---------------------------------------------------------------------------

// vWorldPos is in camera-relative space (uModel has eye pre-subtracted).
// That keeps precision at planet scale and means uCameraPos == 0, which
// simplifies the view-vector math in the fragment shader.
static const char* kMeshVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform int uUseVertexColor;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vCamDist;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(worldPos.xyz);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

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
static const char* kMeshFragSrc = R"(
#version 330 core
#define MAX_LIGHTS 32
#define MAX_SHADOWS 16

in vec3 vWorldPos;   // camera-relative
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vCamDist;

uniform vec4 uColor;           // baseColor RGBA (alpha = mesh transparency)
uniform float uEmissive;       // scalar emissive multiplier (0 = off)
uniform vec3 uEmissiveColor;   // per-mesh emissive tint (linear RGB)
uniform float uMetallic;
uniform float uRoughness;
uniform int uUseVertexColor;
uniform int uUseTexture;
uniform sampler2D uBaseColorTex;

uniform float uFogStart;
uniform float uFogEnd;
uniform vec3 uFogColor;
uniform float uNearClip;

uniform vec3 uAmbient;         // flat ambient (placeholder for IBL)
uniform int uUnlit;            // 1 = skip lighting, output baseColor + emissive

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

void main() {
    if (uNearClip > 0.0 && vCamDist < uNearClip) discard;

    vec3 baseColor;
    float baseAlpha;
    if (uUseTexture == 1) {
        vec4 tex = texture(uBaseColorTex, vUV);
        baseColor = tex.rgb * uColor.rgb;
        baseAlpha = tex.a   * uColor.a;
    } else if (uUseVertexColor == 1) {
        baseColor = vColor.rgb;
        baseAlpha = vColor.a;
    } else {
        baseColor = uColor.rgb;
        baseAlpha = uColor.a;
    }

    if (uUnlit == 1) {
        vec3 color = baseColor;
        if (uFogEnd > 0.0) {
            float fogFactor = clamp((vCamDist - uFogStart) / (uFogEnd - uFogStart), 0.0, 1.0);
            fogFactor = fogFactor * fogFactor;
            color = mix(color, uFogColor, fogFactor);
            baseAlpha = mix(baseAlpha, 0.0, fogFactor);
        }
        FragColor = vec4(color, baseAlpha);
        return;
    }

    vec3 N = normalize(vNormal);
    vec3 V = normalize(-vWorldPos);              // eye at origin (cam-relative)
    float NdotV = max(dot(N, V), 1e-4);

    float rough = clamp(uRoughness, 0.04, 1.0);  // floor to avoid spec singularity
    vec3 F0 = mix(vec3(0.04), baseColor, uMetallic);

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
        vec3 kD = (vec3(1.0) - F) * (1.0 - uMetallic);
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
        }

        vec3 radiance = uLightColor[i] * uLightIntensity[i] * atten;
        Lo += shadow * (diffuse + specular) * radiance * NdotL;
    }

    vec3 ambient = uAmbient * baseColor * (1.0 - uMetallic);
    vec3 emissive = uEmissiveColor * uEmissive;
    vec3 color = Lo + ambient + emissive;

    // Distance fog (applied in linear space; tonemap runs after)
    if (uFogEnd > 0.0) {
        float fogFactor = clamp((vCamDist - uFogStart) / (uFogEnd - uFogStart), 0.0, 1.0);
        fogFactor = fogFactor * fogFactor;
        color = mix(color, uFogColor, fogFactor);
        baseAlpha = mix(baseAlpha, 0.0, fogFactor);
    }

    FragColor = vec4(color, baseAlpha);
}
)";

// -----------------------------------------------------------------------------
// Tonemap pass: HDR float mesh FBO -> LDR RGBA8 output texture.
// -----------------------------------------------------------------------------

static const char* kTonemapVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kTonemapFragSrc = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform float uExposure;
uniform float uGamma;
uniform int   uMode;   // 0 = linear clamp, 1 = Reinhard, 2 = ACES
out vec4 FragColor;

// ACES approximation by Krzysztof Narkowicz.
vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 src = texture(uTex, vUV);
    vec3 c = src.rgb * uExposure;
    if (uMode == 2)      c = aces(c);
    else if (uMode == 1) c = c / (c + vec3(1.0));
    else                 c = clamp(c, 0.0, 1.0);
    if (uGamma > 0.0 && uGamma != 1.0) {
        c = pow(c, vec3(1.0 / uGamma));
    }
    FragColor = vec4(c, src.a);
}
)";

// ---------------------------------------------------------------------------
// Equirectangular HDR → cubemap conversion. One vertex shader (NDC quad),
// one fragment shader that's invoked once per cube face. uFace selects the
// face mapping; gl_FragCoord drives the [-1,1] surface coords.
// ---------------------------------------------------------------------------

static const char* kEnvConvertVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kEnvConvertFragSrc = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uEquirect;
uniform int       uFace;       // 0..5 = +X -X +Y -Y +Z -Z

const float PI     = 3.14159265358979;
const float TWO_PI = 6.28318530717958;

// Cubemap face → world-space direction. Standard GL cube convention.
// uv is in [-1,1] (centre of face = 0,0).
vec3 cubeDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));  // +X
    if (face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));  // -X
    if (face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));  // +Y
    if (face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));  // -Y
    if (face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));  // +Z
    return            normalize(vec3(-uv.x, -uv.y, -1.0));      // -Z
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    vec3 d  = cubeDir(uFace, uv);
    float phi   = atan(d.z, d.x);
    float theta = asin(clamp(d.y, -1.0, 1.0));
    // HDRIs from stb_image are uploaded top-down (row 0 = north pole),
    // so the +Y direction must read v=0. Hence 0.5 - theta/PI.
    vec2 eq = vec2(phi / TWO_PI + 0.5, 0.5 - theta / PI);
    FragColor = vec4(texture(uEquirect, eq).rgb, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Skybox: render the IBL cubemap as the scene background. Reconstructs a
// world-space view direction from NDC + camera FOV/aspect (no matrix
// inverse needed — the view rotation transposed = view→world for the
// orthonormal basis). Drawn first into the HDR FBO with depth-test off
// so geometry simply paints over it.
// ---------------------------------------------------------------------------

static const char* kSkyboxVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec3 vWorldDir;
uniform mat3  uViewToWorld;
uniform float uTanHalfFovY;
uniform float uAspect;
void main() {
    vec3 viewDir = vec3(aPos.x * uTanHalfFovY * uAspect,
                        aPos.y * uTanHalfFovY,
                        -1.0);
    vWorldDir = uViewToWorld * viewDir;
    // z = 1 puts the quad at the far plane — even if depth test were on,
    // this would lose to anything with valid geometry depth.
    gl_Position = vec4(aPos, 1.0, 1.0);
}
)";

static const char* kSkyboxFragSrc = R"(
#version 330 core
in vec3 vWorldDir;
out vec4 FragColor;
uniform samplerCube uEnv;
uniform float uIntensity;
uniform float uRotation;     // Y-axis rotation (radians), positive = clockwise looking down +Y
void main() {
    vec3 d = normalize(vWorldDir);
    float c = cos(uRotation), s = sin(uRotation);
    d = vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
    FragColor = vec4(texture(uEnv, d).rgb * uIntensity, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Shadow caster shader — depth-only. Used to render every shadow-casting
// MeshNode into one tile of the shadow atlas per shadow-casting light. The
// CPU pre-bakes uMVP = lightProj * lightView * meshWorldModel; no other
// material state matters because the FBO writes only depth.
// ---------------------------------------------------------------------------

static const char* kShadowVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* kShadowFragSrc = R"(
#version 330 core
void main() { }
)";

// ---------------------------------------------------------------------------
// Billboard shader sources — a single camera-facing textured/filled quad.
// The vertex shader places a unit quad at a world anchor using camera basis
// vectors supplied by the CPU. Positions are computed in a *camera-relative*
// frame (anchor - cameraEye), which matches renderMeshNode and avoids float
// precision loss at large world coordinates.
// ---------------------------------------------------------------------------

static const char* kBillboardVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aQuad;  // [-1..1] corner

uniform mat4 uVP;            // projection * viewRot (no translation)
uniform vec3 uAnchorRel;     // worldAnchor - cameraEye
uniform vec3 uRight;         // billboard right axis, world space
uniform vec3 uUp;            // billboard up axis, world space
uniform vec2 uHalfSize;      // world-space half-extents

out vec2 vUV;

void main() {
    vec3 worldRel = uAnchorRel
                  + uRight * (aQuad.x * uHalfSize.x)
                  + uUp    * (aQuad.y * uHalfSize.y);
    // Flip Y so UV origin is top-left (matches Skia/CSS pixel layout).
    vUV = vec2(aQuad.x * 0.5 + 0.5, 0.5 - aQuad.y * 0.5);
    gl_Position = uVP * vec4(worldRel, 1.0);
}
)";

static const char* kBillboardFragSrc = R"(
#version 330 core
in vec2 vUV;

uniform int uShapeMode;      // 0 = rect, 1 = circle SDF, 2 = textured, 3 = ringed disc
uniform vec4 uColor;         // rect / circle fill, or tint for texture
uniform vec4 uStroke;
uniform float uStrokeWidth;  // in UV units (0 = no stroke)
uniform sampler2D uTex;

out vec4 FragColor;

void main() {
    if (uShapeMode == 0) {
        // Rect: solid fill with optional inset stroke.
        vec4 c = uColor;
        if (uStrokeWidth > 0.0) {
            vec2 d = min(vUV, 1.0 - vUV);
            float border = min(d.x, d.y);
            if (border < uStrokeWidth) c = uStroke;
        }
        if (c.a <= 0.0) discard;
        // Straight-alpha input — premultiply for "over" blend.
        FragColor = vec4(c.rgb * c.a, c.a);
    } else if (uShapeMode == 1) {
        // Circle SDF centered on UV (0.5, 0.5), radius 0.5.
        vec2 p = vUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float a = uColor.a * alpha;
        FragColor = vec4(uColor.rgb * a, a);
    } else if (uShapeMode == 3) {
        // Filled disc with a ring border. uStrokeWidth is ring thickness as
        // a fraction of the radius (0.2 = outer 20% is ring). Used for
        // engine-drawn gizmo/editor icons (e.g. light markers).
        vec2 p = vUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float inner = 1.0 - clamp(uStrokeWidth, 0.0, 1.0);
        float ringT = smoothstep(inner - aa, inner + aa, d);
        vec4 c = mix(uColor, uStroke, ringT);
        float a = c.a * alpha;
        FragColor = vec4(c.rgb * a, a);
    } else {
        // Textured (premultiplied alpha from Skia surfaces).
        vec4 tex = texture(uTex, vUV);
        vec4 c = tex * uColor; // uColor.a tints premultiplied texture
        if (c.a <= 0.0) discard;
        FragColor = c;
    }
}
)";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SceneGraph::SceneGraph() {
    root_ = std::make_unique<SceneNode>("__root__");
}

SceneGraph::~SceneGraph() {
    root_->traverse([](SceneNode* n) {
        for (auto* c : n->children()) {
        }
    });
    nodes_.clear();

    // Destroy GL resources
    destroyMeshFBO();
    destroyTonemapFBO();
    if (meshProgram_) { glDeleteProgram(meshProgram_); meshProgram_ = 0; }
    if (bbProgram_) { glDeleteProgram(bbProgram_); bbProgram_ = 0; }
    if (bbVBO_) { glDeleteBuffers(1, &bbVBO_); bbVBO_ = 0; }
    if (bbVAO_) { glDeleteVertexArrays(1, &bbVAO_); bbVAO_ = 0; }
    if (tonemapProgram_) { glDeleteProgram(tonemapProgram_); tonemapProgram_ = 0; }
    if (tonemapVBO_) { glDeleteBuffers(1, &tonemapVBO_); tonemapVBO_ = 0; }
    if (tonemapVAO_) { glDeleteVertexArrays(1, &tonemapVAO_); tonemapVAO_ = 0; }
    destroyShadowAtlas();
    if (shadowProgram_) { glDeleteProgram(shadowProgram_); shadowProgram_ = 0; }
    clearEnvironment();
    if (envConvertProgram_) { glDeleteProgram(envConvertProgram_); envConvertProgram_ = 0; }
    if (envConvertVBO_) { glDeleteBuffers(1, &envConvertVBO_); envConvertVBO_ = 0; }
    if (envConvertVAO_) { glDeleteVertexArrays(1, &envConvertVAO_); envConvertVAO_ = 0; }
    if (envConvertFBO_) { glDeleteFramebuffers(1, &envConvertFBO_); envConvertFBO_ = 0; }
    if (skyboxProgram_) { glDeleteProgram(skyboxProgram_); skyboxProgram_ = 0; }
    if (skyboxVBO_) { glDeleteBuffers(1, &skyboxVBO_); skyboxVBO_ = 0; }
    if (skyboxVAO_) { glDeleteVertexArrays(1, &skyboxVAO_); skyboxVAO_ = 0; }
}

SceneNode* SceneGraph::createNode(const std::string& name) {
    auto node = std::make_unique<SceneNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

ShapeNode* SceneGraph::createShape(const std::string& name) {
    auto node = std::make_unique<ShapeNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

SpriteNode* SceneGraph::createSprite(const std::string& name) {
    auto node = std::make_unique<SpriteNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

PhysicsNode* SceneGraph::createPhysicsNode(const std::string& name) {
    auto node = std::make_unique<PhysicsNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

MeshNode* SceneGraph::createMesh(const std::string& name) {
    auto node = std::make_unique<MeshNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

HtmlNode* SceneGraph::createHtml(const std::string& name) {
    auto node = std::make_unique<HtmlNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

LightNode* SceneGraph::createLight(const std::string& name) {
    auto node = std::make_unique<LightNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

void SceneGraph::setCanvasSize(int w, int h) {
    canvasWidth_ = w;
    canvasHeight_ = h;
}

void SceneGraph::collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids) {
    for (auto* child : node->children()) {
        collectDestroyList(child, ids);
    }
    ids.push_back(node->id());
}

void SceneGraph::destroyNode(SceneNode* node) {
    if (!node || node == root_.get()) return;

    // Collect this node and all descendants
    std::vector<uint32_t> ids;
    collectDestroyList(node, ids);

    // Detach from parent first
    node->removeFromParent();

    // Destroy all collected nodes (children first due to ordering)
    for (auto id : ids) {
        agentBindings_.erase(id);
        nodes_.erase(id);
    }
}

SceneNode* SceneGraph::findById(uint32_t id) const {
    auto it = nodes_.find(id);
    return (it != nodes_.end()) ? it->second.get() : nullptr;
}

SceneNode* SceneGraph::findByName(const std::string& name) const {
    for (auto& [id, node] : nodes_) {
        if (node->name() == name) return node.get();
    }
    return nullptr;
}

void SceneGraph::setCamera(float fovY, float aspect, float nearZ, float farZ,
                           const Vec3& eye, const Vec3& target, const Vec3& up) {
    projectionMatrix_ = Mat4::perspective(fovY, aspect, nearZ, farZ);
    viewMatrix_ = Mat4::lookAt(eye, target, up);
    cameraEye_ = eye;
    cameraNearZ_ = nearZ; cameraFarZ_ = farZ; cameraFovY_ = fovY; cameraAspect_ = aspect;
    cameraIsPerspective_ = true;
}

void SceneGraph::setCameraQuat(float fovY, float aspect, float nearZ, float farZ,
                               const Vec3& eye, const Quat& orientation) {
    projectionMatrix_ = Mat4::perspective(fovY, aspect, nearZ, farZ);
    // View matrix = inverse camera transform. For unit quaternion, inverse = conjugate.
    Quat inv = orientation.conjugate();
    viewMatrix_ = Mat4::fromQuat(inv);
    // Translate in rotated space: view translation = inv.rotate(-eye)
    Vec3 negEye = inv.rotate({-eye.x, -eye.y, -eye.z});
    viewMatrix_.m[3][0] = negEye.x;
    viewMatrix_.m[3][1] = negEye.y;
    viewMatrix_.m[3][2] = negEye.z;
    cameraEye_ = eye;
    cameraNearZ_ = nearZ; cameraFarZ_ = farZ; cameraFovY_ = fovY; cameraAspect_ = aspect;
    cameraIsPerspective_ = true;
}

void SceneGraph::setCameraOrtho(float left, float right, float bottom, float top,
                                float nearZ, float farZ,
                                const Vec3& eye, const Vec3& target, const Vec3& up) {
    projectionMatrix_ = Mat4::orthographic(left, right, bottom, top, nearZ, farZ);
    viewMatrix_ = Mat4::lookAt(eye, target, up);
    cameraEye_ = eye;
    cameraNearZ_ = nearZ; cameraFarZ_ = farZ;
    cameraIsPerspective_ = false;
}

void SceneGraph::setCameraPosition(float x, float y) {
    cameraX_ = x;
    cameraY_ = y;
}

void SceneGraph::setCameraZoom(float z) {
    cameraZoom_ = z;
}

void SceneGraph::syncPhysics() {
    if (!physicsWorld_) return;
    for (auto& [id, node] : nodes_) {
        if (node->type() == SceneNode::Type::Physics) {
            auto* pn = static_cast<PhysicsNode*>(node.get());
            if (pn->autoSync()) {
                pn->syncFromPhysics(physicsWorld_);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AI integration
// ---------------------------------------------------------------------------

void SceneGraph::attachAIWorld(brogameagent::World* world, float stepHz, int maxStepsPerFrame) {
    if (!world) { aiTicker_.reset(); return; }
    aiTicker_ = std::make_unique<AIWorldTicker>(world, stepHz, maxStepsPerFrame);
}

void SceneGraph::detachAIWorld() {
    aiTicker_.reset();
}

AgentBinding* SceneGraph::attachAgentBinding(SceneNode* node) {
    if (!node) return nullptr;
    auto it = nodes_.find(node->id());
    if (it == nodes_.end()) return nullptr;
    auto& slot = agentBindings_[node->id()];
    if (!slot) slot = std::make_unique<AgentBinding>(node);
    return slot.get();
}

AgentBinding* SceneGraph::agentBinding(uint32_t nodeId) const {
    auto it = agentBindings_.find(nodeId);
    return (it != agentBindings_.end()) ? it->second.get() : nullptr;
}

AgentBinding* SceneGraph::agentBinding(SceneNode* node) const {
    return node ? agentBinding(node->id()) : nullptr;
}

void SceneGraph::detachAgentBinding(SceneNode* node) {
    if (!node) return;
    agentBindings_.erase(node->id());
}

void SceneGraph::syncAgents(float dt) {
    // 1. Fixed-step the AI world.
    float nowSec = 0.0f;
    brogameagent::World* world = nullptr;
    if (aiTicker_) {
        aiTicker_->tick(dt);
        nowSec = aiTicker_->nowSec();
        world = aiTicker_->world();
    }

    // 2. Step each binding (think + advance + transform sync).
    // Iterate a snapshot of ids — a think() callback could theoretically
    // spawn/destroy bindings, invalidating the map's iterators.
    if (agentBindings_.empty()) return;
    std::vector<uint32_t> ids;
    ids.reserve(agentBindings_.size());
    for (const auto& [id, _] : agentBindings_) ids.push_back(id);
    for (uint32_t id : ids) {
        auto it = agentBindings_.find(id);
        if (it == agentBindings_.end()) continue;
        if (AgentBinding* b = it->second.get()) {
            b->step(world, dt, nowSec);
        }
    }
}

// ---------------------------------------------------------------------------
// Mesh GL pipeline (lazy init)
// ---------------------------------------------------------------------------

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR("Mesh shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void SceneGraph::ensureMeshPipeline() {
    if (meshProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kMeshVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kMeshFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    meshProgram_ = glCreateProgram();
    glAttachShader(meshProgram_, vs);
    glAttachShader(meshProgram_, fs);
    glLinkProgram(meshProgram_);

    GLint ok = 0;
    glGetProgramiv(meshProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(meshProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Mesh program link error: %s", log);
        glDeleteProgram(meshProgram_);
        meshProgram_ = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (meshProgram_) {
        uMVP_ = glGetUniformLocation(meshProgram_, "uMVP");
        uModel_ = glGetUniformLocation(meshProgram_, "uModel");
        uColor_ = glGetUniformLocation(meshProgram_, "uColor");
        uEmissive_ = glGetUniformLocation(meshProgram_, "uEmissive");
        uEmissiveColor_ = glGetUniformLocation(meshProgram_, "uEmissiveColor");
        uMetallic_ = glGetUniformLocation(meshProgram_, "uMetallic");
        uRoughness_ = glGetUniformLocation(meshProgram_, "uRoughness");
        uUseVertexColor_ = glGetUniformLocation(meshProgram_, "uUseVertexColor");
        uUseTexture_     = glGetUniformLocation(meshProgram_, "uUseTexture");
        uBaseColorTex_   = glGetUniformLocation(meshProgram_, "uBaseColorTex");
        uFogStart_ = glGetUniformLocation(meshProgram_, "uFogStart");
        uFogEnd_ = glGetUniformLocation(meshProgram_, "uFogEnd");
        uFogColor_ = glGetUniformLocation(meshProgram_, "uFogColor");
        uNearClip_ = glGetUniformLocation(meshProgram_, "uNearClip");
        uAmbient_ = glGetUniformLocation(meshProgram_, "uAmbient");
        uUnlit_   = glGetUniformLocation(meshProgram_, "uUnlit");
        uLightCount_ = glGetUniformLocation(meshProgram_, "uLightCount");
        uLightType_ = glGetUniformLocation(meshProgram_, "uLightType");
        uLightPos_ = glGetUniformLocation(meshProgram_, "uLightPos");
        uLightDirArr_ = glGetUniformLocation(meshProgram_, "uLightDir");
        uLightColor_ = glGetUniformLocation(meshProgram_, "uLightColor");
        uLightIntensity_ = glGetUniformLocation(meshProgram_, "uLightIntensity");
        uLightRange_ = glGetUniformLocation(meshProgram_, "uLightRange");
        uLightSpotCos_ = glGetUniformLocation(meshProgram_, "uLightSpotCos");
        uLightShadowSlot_  = glGetUniformLocation(meshProgram_, "uLightShadowSlot");
        uLightShadowSlotCount_ = glGetUniformLocation(meshProgram_, "uLightShadowSlotCount");
        uLightCascadeSplit_    = glGetUniformLocation(meshProgram_, "uLightCascadeSplit");
        uShadowAtlas_      = glGetUniformLocation(meshProgram_, "uShadowAtlas");
        uShadowMatrix_     = glGetUniformLocation(meshProgram_, "uShadowMatrix");
        uShadowAtlasRect_  = glGetUniformLocation(meshProgram_, "uShadowAtlasRect");
        uShadowBiasArr_    = glGetUniformLocation(meshProgram_, "uShadowBias");
        uShadowAtlasTexel_ = glGetUniformLocation(meshProgram_, "uShadowAtlasTexel");
        uShadowPCFTaps_    = glGetUniformLocation(meshProgram_, "uShadowPCFTaps");
        // Legacy — no longer declared in the shader, fine if -1.
        uLightDir_ = -1;
        uCameraPos_ = -1;
    }
}

void SceneGraph::ensureMeshFBO() {
    if (canvasWidth_ <= 0 || canvasHeight_ <= 0) return;
    if (meshFBO_ && meshFBOWidth_ == canvasWidth_ && meshFBOHeight_ == canvasHeight_) return;

    destroyMeshFBO();

    meshFBOWidth_ = canvasWidth_;
    meshFBOHeight_ = canvasHeight_;

    glGenFramebuffers(1, &meshFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, meshFBO_);

    // HDR color attachment — RGBA16F so lighting can exceed 1.0 before
    // tonemap. The LDR output texture consumed by the compositor is a
    // separate RGBA8 texture owned by the tonemap FBO.
    glGenTextures(1, &meshColorTex_);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, meshFBOWidth_, meshFBOHeight_, 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, meshColorTex_, 0);

    // Depth renderbuffer
    glGenRenderbuffers(1, &meshDepthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, meshDepthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, meshFBOWidth_, meshFBOHeight_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, meshDepthRBO_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Mesh FBO incomplete: 0x%x", status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneGraph::destroyMeshFBO() {
    if (meshDepthRBO_) { glDeleteRenderbuffers(1, &meshDepthRBO_); meshDepthRBO_ = 0; }
    if (meshColorTex_) { glDeleteTextures(1, &meshColorTex_); meshColorTex_ = 0; }
    if (meshFBO_) { glDeleteFramebuffers(1, &meshFBO_); meshFBO_ = 0; }
    meshFBOWidth_ = 0;
    meshFBOHeight_ = 0;
}

void SceneGraph::ensureBillboardPipeline() {
    if (bbProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kBillboardVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kBillboardFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    bbProgram_ = glCreateProgram();
    glAttachShader(bbProgram_, vs);
    glAttachShader(bbProgram_, fs);
    glLinkProgram(bbProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(bbProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(bbProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Billboard program link error: %s", log);
        glDeleteProgram(bbProgram_);
        bbProgram_ = 0;
        return;
    }

    bbUVP_         = glGetUniformLocation(bbProgram_, "uVP");
    bbUAnchorRel_  = glGetUniformLocation(bbProgram_, "uAnchorRel");
    bbURight_      = glGetUniformLocation(bbProgram_, "uRight");
    bbUUp_         = glGetUniformLocation(bbProgram_, "uUp");
    bbUHalfSize_   = glGetUniformLocation(bbProgram_, "uHalfSize");
    bbUShapeMode_  = glGetUniformLocation(bbProgram_, "uShapeMode");
    bbUColor_      = glGetUniformLocation(bbProgram_, "uColor");
    bbUStroke_     = glGetUniformLocation(bbProgram_, "uStroke");
    bbUStrokeWidth_ = glGetUniformLocation(bbProgram_, "uStrokeWidth");
    bbUTex_        = glGetUniformLocation(bbProgram_, "uTex");

    // Two triangles covering [-1,1] on both axes. Shared across every
    // billboard — only uniforms change per draw.
    static const float quadVerts[12] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &bbVAO_);
    glGenBuffers(1, &bbVBO_);
    glBindVertexArray(bbVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, bbVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Tonemap pipeline + FBO
// ---------------------------------------------------------------------------

void SceneGraph::ensureTonemapPipeline() {
    if (tonemapProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kTonemapVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kTonemapFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    tonemapProgram_ = glCreateProgram();
    glAttachShader(tonemapProgram_, vs);
    glAttachShader(tonemapProgram_, fs);
    glLinkProgram(tonemapProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(tonemapProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(tonemapProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Tonemap program link error: %s", log);
        glDeleteProgram(tonemapProgram_);
        tonemapProgram_ = 0;
        return;
    }

    tmUTex_      = glGetUniformLocation(tonemapProgram_, "uTex");
    tmUExposure_ = glGetUniformLocation(tonemapProgram_, "uExposure");
    tmUGamma_    = glGetUniformLocation(tonemapProgram_, "uGamma");
    tmUMode_     = glGetUniformLocation(tonemapProgram_, "uMode");

    static const float quadVerts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &tonemapVAO_);
    glGenBuffers(1, &tonemapVBO_);
    glBindVertexArray(tonemapVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, tonemapVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);
}

void SceneGraph::ensureTonemapFBO() {
    if (canvasWidth_ <= 0 || canvasHeight_ <= 0) return;
    if (tonemapFBO_ && tonemapFBOWidth_ == canvasWidth_
                   && tonemapFBOHeight_ == canvasHeight_) return;

    destroyTonemapFBO();

    tonemapFBOWidth_  = canvasWidth_;
    tonemapFBOHeight_ = canvasHeight_;

    glGenFramebuffers(1, &tonemapFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);

    glGenTextures(1, &tonemapColorTex_);
    glBindTexture(GL_TEXTURE_2D, tonemapColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tonemapFBOWidth_, tonemapFBOHeight_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tonemapColorTex_, 0);

    // Reuse the mesh FBO's depth-stencil RBO so the post-tonemap unlit overlay
    // pass can depth-test against the scene geometry that was rendered there.
    if (meshDepthRBO_) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, meshDepthRBO_);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Tonemap FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneGraph::destroyTonemapFBO() {
    if (tonemapColorTex_) { glDeleteTextures(1, &tonemapColorTex_); tonemapColorTex_ = 0; }
    if (tonemapFBO_)      { glDeleteFramebuffers(1, &tonemapFBO_); tonemapFBO_ = 0; }
    tonemapFBOWidth_ = 0;
    tonemapFBOHeight_ = 0;
}

void SceneGraph::runTonemapPass() {
    ensureTonemapPipeline();
    ensureTonemapFBO();
    if (!tonemapProgram_ || !tonemapFBO_) return;

    glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);
    glViewport(0, 0, tonemapFBOWidth_, tonemapFBOHeight_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(tonemapProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glUniform1i(tmUTex_, 0);
    glUniform1f(tmUExposure_, exposure_);
    glUniform1f(tmUGamma_, gamma_);
    glUniform1i(tmUMode_, static_cast<int>(toneMap_));

    glBindVertexArray(tonemapVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// IBL: HDR equirect → cubemap conversion. The cubemap produced here is the
// raw radiance source; later passes (irradiance convolution, prefiltered
// specular) consume it to populate the IBL data the PBR shader samples.
// ---------------------------------------------------------------------------

void SceneGraph::ensureEnvConvertPipeline() {
    if (envConvertProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kEnvConvertVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kEnvConvertFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    envConvertProgram_ = glCreateProgram();
    glAttachShader(envConvertProgram_, vs);
    glAttachShader(envConvertProgram_, fs);
    glLinkProgram(envConvertProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(envConvertProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(envConvertProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Env convert program link error: %s", log);
        glDeleteProgram(envConvertProgram_);
        envConvertProgram_ = 0;
        return;
    }
    envCvUFace_     = glGetUniformLocation(envConvertProgram_, "uFace");
    envCvUEquirect_ = glGetUniformLocation(envConvertProgram_, "uEquirect");

    static const float quadVerts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &envConvertVAO_);
    glGenBuffers(1, &envConvertVBO_);
    glBindVertexArray(envConvertVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, envConvertVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);

    glGenFramebuffers(1, &envConvertFBO_);
}

bool SceneGraph::runEquirectToCubemap(GLuint equirectTex, GLuint cubemap, int faceSize) {
    ensureEnvConvertPipeline();
    if (!envConvertProgram_ || !envConvertFBO_) return false;

    // Save state we touch so the caller's render flow isn't disturbed.
    GLint prevFBO = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, envConvertFBO_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, faceSize, faceSize);

    glUseProgram(envConvertProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, equirectTex);
    glUniform1i(envCvUEquirect_, 0);
    glBindVertexArray(envConvertVAO_);

    bool ok = true;
    for (int face = 0; face < 6; ++face) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemap, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Env convert FBO incomplete on face %d: 0x%x", face, status);
            ok = false;
            break;
        }
        glUniform1i(envCvUFace_, face);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return ok;
}

bool SceneGraph::loadEnvironment(const std::string& hdrPath) {
    if (hdrPath.empty()) {
        clearEnvironment();
        return true;
    }

    int w = 0, h = 0, ch = 0;
    // stbi_loadf returns top-down float RGB(A); the convert shader's UV
    // mapping (`0.5 - theta/PI`) is paired with this orientation.
    float* data = stbi_loadf(hdrPath.c_str(), &w, &h, &ch, 3);
    if (!data) {
        LOG_ERROR("loadEnvironment: stbi_loadf failed for '%s': %s",
                  hdrPath.c_str(), stbi_failure_reason());
        return false;
    }

    // Upload the equirect as a temp 2D float texture.
    GLuint equirectTex = 0;
    glGenTextures(1, &equirectTex);
    glBindTexture(GL_TEXTURE_2D, equirectTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(data);

    // (Re)allocate the destination cubemap. 512² is enough for a sharp
    // skybox; prefilter mips will be derived from this in a later pass so
    // we allocate the full mip chain upfront.
    const int faceSize = 512;
    if (envCubemap_ && envCubemapSize_ != faceSize) {
        glDeleteTextures(1, &envCubemap_);
        envCubemap_ = 0;
    }
    if (!envCubemap_) {
        glGenTextures(1, &envCubemap_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
        for (int f = 0; f < 6; ++f) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                         faceSize, faceSize, 0, GL_RGBA, GL_FLOAT, nullptr);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        envCubemapSize_ = faceSize;
    }

    bool ok = runEquirectToCubemap(equirectTex, envCubemap_, faceSize);
    glDeleteTextures(1, &equirectTex);
    if (!ok) {
        // Don't keep a half-baked cubemap.
        glDeleteTextures(1, &envCubemap_);
        envCubemap_ = 0;
        envCubemapSize_ = 0;
        envPath_.clear();
        return false;
    }

    // Generate mips so trilinear sampling at low LOD looks clean (and so
    // the prefilter pass has somewhere to write its roughness chain).
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    envPath_ = hdrPath;
    LOG_INFO("Loaded HDR environment '%s' (%dx%d → cube %d²)",
             hdrPath.c_str(), w, h, faceSize);
    return true;
}

void SceneGraph::clearEnvironment() {
    if (envCubemap_) { glDeleteTextures(1, &envCubemap_); envCubemap_ = 0; }
    envCubemapSize_ = 0;
    envPath_.clear();
}

void SceneGraph::ensureSkyboxPipeline() {
    if (skyboxProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kSkyboxVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kSkyboxFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    skyboxProgram_ = glCreateProgram();
    glAttachShader(skyboxProgram_, vs);
    glAttachShader(skyboxProgram_, fs);
    glLinkProgram(skyboxProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(skyboxProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(skyboxProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Skybox program link error: %s", log);
        glDeleteProgram(skyboxProgram_);
        skyboxProgram_ = 0;
        return;
    }
    skyUViewToWorld_ = glGetUniformLocation(skyboxProgram_, "uViewToWorld");
    skyUTanHalfFovY_ = glGetUniformLocation(skyboxProgram_, "uTanHalfFovY");
    skyUAspect_      = glGetUniformLocation(skyboxProgram_, "uAspect");
    skyUEnv_         = glGetUniformLocation(skyboxProgram_, "uEnv");
    skyUIntensity_   = glGetUniformLocation(skyboxProgram_, "uIntensity");
    skyURotation_    = glGetUniformLocation(skyboxProgram_, "uRotation");

    static const float quadVerts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &skyboxVAO_);
    glGenBuffers(1, &skyboxVBO_);
    glBindVertexArray(skyboxVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);
}

void SceneGraph::renderSkyboxPass() {
    if (!envCubemap_) return;
    if (!cameraIsPerspective_) return;  // Ortho cameras have no view direction.
    ensureSkyboxPipeline();
    if (!skyboxProgram_) return;

    // viewMatrix_ stores world→view (column-major). The 3x3 rotation block
    // is orthonormal (lookAt produces it), so its transpose is its inverse
    // and gives view→world. Pass that to the shader as a mat3.
    float viewToWorld[9] = {
        viewMatrix_.m[0][0], viewMatrix_.m[0][1], viewMatrix_.m[0][2],
        viewMatrix_.m[1][0], viewMatrix_.m[1][1], viewMatrix_.m[1][2],
        viewMatrix_.m[2][0], viewMatrix_.m[2][1], viewMatrix_.m[2][2],
    };
    // GLSL mat3 columns are: column 0 = view→world basis vector for view-X.
    // viewMatrix's row 0 (m[0..2][0]) is the world-space camera-right vector,
    // which is exactly view-X→world. So packing rows-of-view as cols-of-m3
    // gives the transpose we want. The pack above does that.

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(skyboxProgram_);
    glUniformMatrix3fv(skyUViewToWorld_, 1, GL_FALSE, viewToWorld);
    glUniform1f(skyUTanHalfFovY_, std::tan(cameraFovY_ * 0.5f));
    glUniform1f(skyUAspect_, cameraAspect_);
    glUniform1f(skyUIntensity_, envIntensity_);
    glUniform1f(skyURotation_, envRotation_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
    glUniform1i(skyUEnv_, 0);

    glBindVertexArray(skyboxVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    // Re-enable depth write/test for the geometry passes that follow.
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// Light collection + upload
// ---------------------------------------------------------------------------

void SceneGraph::collectLights(std::vector<LightNode*>& out) const {
    out.clear();
    for (auto& [id, node] : nodes_) {
        if (!node->visible()) continue;
        if (node->type() != SceneNode::Type::Light) continue;
        // Include only nodes actually attached to the tree (parent chain ends
        // at root_). Detached lights created but never added shouldn't light.
        SceneNode* p = node.get();
        while (p && p->parent()) p = p->parent();
        if (p != root_.get()) continue;
        out.push_back(static_cast<LightNode*>(node.get()));
        if (out.size() >= 32) break;
    }
}

void SceneGraph::uploadLights(const std::vector<LightNode*>& lights) {
    const int count = std::min((int)lights.size(), 32);
    glUniform1i(uLightCount_, count);

    // Always upload shadow uniforms (even when no lights / no shadows): the
    // shader unconditionally indexes them per-iteration. Texel + tap config
    // is global so set them once per draw regardless of light count.
    if (uShadowAtlasTexel_ >= 0) {
        float texel = (shadowAtlasSize_ > 0) ? (1.0f / (float)shadowAtlasSize_) : 0.0f;
        glUniform1f(uShadowAtlasTexel_, texel);
    }
    if (uShadowPCFTaps_ >= 0) glUniform1i(uShadowPCFTaps_, shadowPCFTaps_);

    // Bind the shadow atlas to a fixed texture unit (1; unit 0 is baseColor).
    // sampler2DShadow performs the depth comparison via the texture's
    // GL_TEXTURE_COMPARE_MODE state set in ensureShadowAtlas().
    if (uShadowAtlas_ >= 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, shadowAtlasTex_);
        glUniform1i(uShadowAtlas_, 1);
        glActiveTexture(GL_TEXTURE0);
    }

    if (uShadowMatrix_ >= 0 && shadowTileCount_ > 0) {
        glUniformMatrix4fv(uShadowMatrix_, shadowTileCount_, GL_FALSE,
                           &shadowMatrixCamRel_[0][0]);
    }
    if (uShadowAtlasRect_ >= 0 && shadowTileCount_ > 0) {
        glUniform4fv(uShadowAtlasRect_, shadowTileCount_, &shadowAtlasRect_[0][0]);
    }
    if (uShadowBiasArr_ >= 0 && shadowTileCount_ > 0) {
        glUniform2fv(uShadowBiasArr_, shadowTileCount_, &shadowBias_[0][0]);
    }
    if (uLightShadowSlot_ >= 0) {
        // Always send 32 slots so any light index is safe to read; -1 default.
        glUniform1iv(uLightShadowSlot_, 32, lightShadowSlot_);
    }
    if (uLightShadowSlotCount_ >= 0) {
        glUniform1iv(uLightShadowSlotCount_, 32, lightShadowSlotCount_);
    }
    if (uLightCascadeSplit_ >= 0) {
        glUniform4fv(uLightCascadeSplit_, 32, &lightCascadeSplit_[0][0]);
    }

    if (count == 0) return;

    int   type[32];
    float pos[32 * 3];
    float dir[32 * 3];
    float col[32 * 3];
    float intensity[32];
    float range[32];
    float spotCos[32 * 2];

    for (int i = 0; i < count; ++i) {
        LightNode* L = lights[i];
        type[i] = static_cast<int>(L->kind());

        // Light world position (column-major translation column), then
        // made camera-relative to match vWorldPos in the fragment shader.
        const Mat4& M = L->worldMatrix();
        Vec3 rel { M.m[3][0] - cameraEye_.x,
                   M.m[3][1] - cameraEye_.y,
                   M.m[3][2] - cameraEye_.z };

        Vec3 d = L->direction();
        float dlen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        if (dlen > 1e-6f) { d.x /= dlen; d.y /= dlen; d.z /= dlen; }

        pos[i*3+0] = rel.x; pos[i*3+1] = rel.y; pos[i*3+2] = rel.z;
        dir[i*3+0] = d.x;   dir[i*3+1] = d.y;   dir[i*3+2] = d.z;

        const Vec3& c = L->color();
        col[i*3+0] = c.x; col[i*3+1] = c.y; col[i*3+2] = c.z;

        intensity[i] = L->intensity();
        range[i]     = L->range();

        // Pre-compute spot cos-angles (shader compares cos directly).
        spotCos[i*2+0] = std::cos(L->innerAngle());
        spotCos[i*2+1] = std::cos(L->outerAngle());
    }

    glUniform1iv(uLightType_, count, type);
    glUniform3fv(uLightPos_, count, pos);
    glUniform3fv(uLightDirArr_, count, dir);
    glUniform3fv(uLightColor_, count, col);
    glUniform1fv(uLightIntensity_, count, intensity);
    glUniform1fv(uLightRange_, count, range);
    glUniform2fv(uLightSpotCos_, count, spotCos);
}

// ---------------------------------------------------------------------------
// Shadow pipeline
// ---------------------------------------------------------------------------

void SceneGraph::ensureShadowPipeline() {
    if (shadowProgram_) return;
    GLuint vs = compileShader(GL_VERTEX_SHADER,   kShadowVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kShadowFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    shadowProgram_ = glCreateProgram();
    glAttachShader(shadowProgram_, vs);
    glAttachShader(shadowProgram_, fs);
    glLinkProgram(shadowProgram_);
    GLint ok = 0;
    glGetProgramiv(shadowProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(shadowProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Shadow program link error: %s", log);
        glDeleteProgram(shadowProgram_);
        shadowProgram_ = 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    if (shadowProgram_) {
        shadowUMVP_ = glGetUniformLocation(shadowProgram_, "uMVP");
    }
}

void SceneGraph::ensureShadowAtlas() {
    if (shadowAtlasTex_ && shadowAtlasAllocated_ == shadowAtlasSize_ && !shadowAtlasDirty_) return;
    destroyShadowAtlas();
    shadowAtlasAllocated_ = shadowAtlasSize_;
    shadowAtlasDirty_ = false;

    glGenTextures(1, &shadowAtlasTex_);
    glBindTexture(GL_TEXTURE_2D, shadowAtlasTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 shadowAtlasSize_, shadowAtlasSize_, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Hardware PCF: sampler2DShadow returns a [0,1] comparison result and
    // bilinearly filters between neighbouring texels — much cheaper than
    // four manual texture() lookups, and visually identical for 2x2 PCF.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    // Atlas-edge sampling reads "infinitely far" depth, i.e. lit. Combined
    // with the in-tile clamp in sampleShadow() this avoids cross-tile bleed.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

    glGenFramebuffers(1, &shadowAtlasFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowAtlasFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, shadowAtlasTex_, 0);
    // No color buffer — depth-only.
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Shadow atlas FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneGraph::destroyShadowAtlas() {
    if (shadowAtlasFBO_) { glDeleteFramebuffers(1, &shadowAtlasFBO_); shadowAtlasFBO_ = 0; }
    if (shadowAtlasTex_) { glDeleteTextures(1, &shadowAtlasTex_); shadowAtlasTex_ = 0; }
    shadowAtlasAllocated_ = 0;
}

SceneGraph::WorldAABB SceneGraph::computeShadowCasterBounds() const {
    WorldAABB out{};
    out.empty = true;
    out.min[0] = out.min[1] = out.min[2] =  1e30f;
    out.max[0] = out.max[1] = out.max[2] = -1e30f;

    auto walk = [&](auto&& self, SceneNode* n) -> void {
        if (!n || !n->visible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            if (!m->unlit() && !m->mesh().empty()) {
                const auto& bb = m->localBounds();
                const Mat4& M = m->worldMatrix();
                // Transform the eight corners of the local AABB to world.
                for (int c = 0; c < 8; ++c) {
                    Vec3 lp{
                        (c & 1) ? bb.max[0] : bb.min[0],
                        (c & 2) ? bb.max[1] : bb.min[1],
                        (c & 4) ? bb.max[2] : bb.min[2],
                    };
                    Vec3 wp = M.transformPoint(lp);
                    out.min[0] = std::min(out.min[0], wp.x);
                    out.min[1] = std::min(out.min[1], wp.y);
                    out.min[2] = std::min(out.min[2], wp.z);
                    out.max[0] = std::max(out.max[0], wp.x);
                    out.max[1] = std::max(out.max[1], wp.y);
                    out.max[2] = std::max(out.max[2], wp.z);
                    out.empty = false;
                }
            }
        }
        for (auto* c : n->children()) self(self, c);
    };
    walk(walk, root_.get());
    return out;
}

void SceneGraph::prepareShadows(const std::vector<LightNode*>& lights) {
    // Reset per-frame shadow state. Default every light to "no shadow".
    shadowTileCount_ = 0;
    shadowCasters_.clear();
    for (int i = 0; i < 32; ++i) {
        lightShadowSlot_[i] = -1;
        lightShadowSlotCount_[i] = 0;
        lightCascadeSplit_[i][0] = lightCascadeSplit_[i][1] =
        lightCascadeSplit_[i][2] = lightCascadeSplit_[i][3] = 1e30f;
    }

    // Quick skip: if no light wants shadows, don't bother fitting.
    bool anyCaster = false;
    for (auto* L : lights) {
        if (L && L->castsShadow()) { anyCaster = true; break; }
    }
    if (!anyCaster) return;

    // Gather shadow-casting meshes once. Unlit meshes never cast.
    auto gather = [&](auto&& self, SceneNode* n) -> void {
        if (!n || !n->visible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            if (!m->unlit() && !m->mesh().empty()) shadowCasters_.push_back(m);
        }
        for (auto* c : n->children()) self(self, c);
    };
    gather(gather, root_.get());
    if (shadowCasters_.empty()) return;

    // Scene bounds for fitting directional frustums. CSM uses view-frustum
    // slices instead — added in a follow-up commit.
    WorldAABB bounds = computeShadowCasterBounds();
    if (bounds.empty) return;

    // Bias matrix maps NDC [-1,1] to UV [0,1] in all three dims.
    Mat4 bias = Mat4::translate(0.5f, 0.5f, 0.5f) * Mat4::scale(0.5f, 0.5f, 0.5f);

    // Allocate atlas tiles in a square grid: ceil(sqrt(MAX)) x ceil(sqrt(MAX)).
    // For MAX=16 this gives a clean 4x4. Each tile gets equal area.
    const int gridDim = 4;                     // 4x4 = 16 tiles
    const float tileUV = 1.0f / (float)gridDim; // 0.25 per tile

    auto bakeTile = [&](int slot, const Mat4& lightProjView, LightNode* L) {
        // shadowMatrixCamRel = bias * proj * view * translate(cameraEye)
        // so the FS can multiply directly against vWorldPos (camera-relative).
        Mat4 t = Mat4::translate(cameraEye_.x, cameraEye_.y, cameraEye_.z);
        Mat4 cam = bias * lightProjView * t;
        std::memcpy(shadowMatrixCamRel_[slot], cam.data(), sizeof(float) * 16);
        std::memcpy(shadowRenderMatrix_[slot], lightProjView.data(), sizeof(float) * 16);

        int gx = slot % gridDim;
        int gy = slot / gridDim;
        shadowAtlasRect_[slot][0] = gx * tileUV;
        shadowAtlasRect_[slot][1] = gy * tileUV;
        shadowAtlasRect_[slot][2] = tileUV;
        shadowAtlasRect_[slot][3] = tileUV;

        shadowBias_[slot][0] = L->shadowBias();
        shadowBias_[slot][1] = L->shadowNormalBias();

        shadowTileLight_[slot] = L;
    };

    // For each shadow-casting light, allocate slot(s) and build matrices.
    // Spot/Point are deferred to follow-up commits — only Directional fits
    // the scene-bounds-ortho path here.
    for (int i = 0; i < (int)lights.size() && i < 32; ++i) {
        LightNode* L = lights[i];
        if (!L || !L->castsShadow()) continue;
        if (shadowTileCount_ >= kMaxShadowTiles) break;

        if (L->kind() == LightNode::Kind::Directional) {
            Vec3 d = L->direction();
            float dlen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            if (dlen < 1e-6f) continue;
            d.x /= dlen; d.y /= dlen; d.z /= dlen;

            // Camera basis from view matrix (transposed columns: row 0 = right,
            // row 1 = up, row 2 = -forward). lookAt produces the same.
            Vec3 sBasis{viewMatrix_.m[0][0], viewMatrix_.m[1][0], viewMatrix_.m[2][0]};
            Vec3 uBasis{viewMatrix_.m[0][1], viewMatrix_.m[1][1], viewMatrix_.m[2][1]};
            Vec3 fBasis{-viewMatrix_.m[0][2], -viewMatrix_.m[1][2], -viewMatrix_.m[2][2]};

            // Number of cascades. Cap to remaining tile budget so we don't
            // blow past the atlas — better to drop late cascades than to
            // silently corrupt allocations.
            int N = L->cascadeCount();
            if (cameraIsPerspective_ == false) N = 1;  // ortho cam = no need for splits
            int budgetLeft = kMaxShadowTiles - shadowTileCount_;
            if (N > budgetLeft) N = budgetLeft;
            if (N <= 0) continue;

            // Practical Split Scheme (Zhang et al., 2006): blend uniform and
            // log spacing. lambda ~0.5 works well outdoors; closer to 0
            // for tight indoor scenes with no far geometry.
            float zn = std::max(cameraNearZ_, 1e-3f);
            float zf = std::max(cameraFarZ_, zn + 1e-3f);
            float lambda = L->cascadeSplitLambda();
            // splitFar[c] = far view-distance of cascade c. cascade c covers
            // [splitFar[c-1], splitFar[c]], with splitFar[-1] = zn.
            float splitFar[5]; splitFar[0] = zn;
            for (int c = 1; c <= N; ++c) {
                float t = (float)c / (float)N;
                float uniform = zn + (zf - zn) * t;
                float logS    = zn * std::pow(zf / zn, t);
                splitFar[c] = lambda * logS + (1.0f - lambda) * uniform;
            }

            int firstSlot = shadowTileCount_;
            lightShadowSlot_[i] = firstSlot;
            lightShadowSlotCount_[i] = N;
            for (int c = 0; c < N - 1; ++c) {
                lightCascadeSplit_[i][c] = splitFar[c + 1];
            }
            // The last cascade absorbs anything farther — already 1e30f from reset.

            // Per-cascade fit: find the world-space corners of the camera
            // sub-frustum [splitFar[c], splitFar[c+1]], then bound them
            // with a sphere (rotation-stable; eliminates shimmer when the
            // camera turns) and fit an ortho frustum in the light's view.
            for (int c = 0; c < N; ++c) {
                float zNear = splitFar[c];
                float zFar  = splitFar[c + 1];
                float tanH  = std::tan(cameraFovY_ * 0.5f);

                Vec3 corners[8];
                for (int k = 0; k < 2; ++k) {
                    float z  = (k == 0) ? zNear : zFar;
                    float hh = z * tanH;
                    float hw = hh * cameraAspect_;
                    Vec3 cz{cameraEye_.x + fBasis.x * z,
                            cameraEye_.y + fBasis.y * z,
                            cameraEye_.z + fBasis.z * z};
                    for (int j = 0; j < 4; ++j) {
                        float xs = (j & 1) ? 1.0f : -1.0f;
                        float ys = (j & 2) ? 1.0f : -1.0f;
                        corners[k*4 + j] = Vec3{
                            cz.x + sBasis.x * (hw * xs) + uBasis.x * (hh * ys),
                            cz.y + sBasis.y * (hw * xs) + uBasis.y * (hh * ys),
                            cz.z + sBasis.z * (hw * xs) + uBasis.z * (hh * ys)};
                    }
                }

                Vec3 center{0,0,0};
                for (int k = 0; k < 8; ++k) {
                    center.x += corners[k].x;
                    center.y += corners[k].y;
                    center.z += corners[k].z;
                }
                center.x *= 0.125f; center.y *= 0.125f; center.z *= 0.125f;

                float radius = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    float dx = corners[k].x - center.x;
                    float dy = corners[k].y - center.y;
                    float dz = corners[k].z - center.z;
                    radius = std::max(radius, std::sqrt(dx*dx + dy*dy + dz*dz));
                }
                if (radius < 1e-3f) radius = 1.0f;
                // Snap radius to 16ths of a unit so it doesn't change every
                // micro-frame; combined with sphere fit this is the second
                // half of the texel-snap shimmer fix.
                radius = std::ceil(radius * 16.0f) / 16.0f;

                // Light-space view: looking from above the bounding sphere
                // along the light direction, looking AT the sphere center.
                Vec3 eye{ center.x - d.x * radius * 2.0f,
                          center.y - d.y * radius * 2.0f,
                          center.z - d.z * radius * 2.0f };
                Vec3 up = (std::abs(d.y) > 0.99f) ? Vec3{0,0,1} : Vec3{0,1,0};
                Mat4 view = Mat4::lookAt(eye, center, up);

                // Texel-snap the cascade origin in light-space xy. Without
                // this the shadow edges shimmer as the camera moves because
                // the same world fragment maps to slightly different texels
                // each frame. Snap the world center, not the projection.
                int tilePx = shadowAtlasSize_ / 4;
                float texelSize = (2.0f * radius) / (float)tilePx;
                Vec3 centerLS = view.transformPoint(center);
                float snapX = std::floor(centerLS.x / texelSize) * texelSize;
                float snapY = std::floor(centerLS.y / texelSize) * texelSize;
                float dxLS = centerLS.x - snapX;
                float dyLS = centerLS.y - snapY;
                // Build ortho extents around the snapped origin.
                Mat4 proj = Mat4::orthographic(
                    -radius - dxLS, radius - dxLS,
                    -radius - dyLS, radius - dyLS,
                    -radius * 2.0f - radius, -(-radius * 2.0f) + radius);
                // Expand the depth range: scene casters outside the sphere
                // should still write their depths (otherwise close objects
                // behind the cascade get omitted from the shadow). Use the
                // scene AABB extent along the light direction as an extra
                // pad on the near side.
                Vec3 boundsCenter{
                    0.5f * (bounds.min[0] + bounds.max[0]),
                    0.5f * (bounds.min[1] + bounds.max[1]),
                    0.5f * (bounds.min[2] + bounds.max[2])};
                Vec3 boundsExt{
                    0.5f * (bounds.max[0] - bounds.min[0]),
                    0.5f * (bounds.max[1] - bounds.min[1]),
                    0.5f * (bounds.max[2] - bounds.min[2])};
                float sceneRadius = std::sqrt(boundsExt.x*boundsExt.x +
                                              boundsExt.y*boundsExt.y +
                                              boundsExt.z*boundsExt.z);
                float depthExt = std::max(sceneRadius * 2.0f, radius * 4.0f);
                proj = Mat4::orthographic(
                    -radius - dxLS, radius - dxLS,
                    -radius - dyLS, radius - dyLS,
                    0.0f, depthExt);

                Mat4 projView = proj * view;
                bakeTile(firstSlot + c, projView, L);
                shadowTileCount_++;
            }
        }
        else if (L->kind() == LightNode::Kind::Spot) {
            // Spot light shadow = perspective projection from the light's
            // position along its direction. FOV = 2 * outerAngle so the
            // shadow frustum exactly covers the cone the FS computes
            // attenuation for. Range determines the far plane.
            Vec3 d = L->direction();
            float dlen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            if (dlen < 1e-6f) continue;
            d.x /= dlen; d.y /= dlen; d.z /= dlen;

            const Mat4& M = L->worldMatrix();
            Vec3 eye{M.m[3][0], M.m[3][1], M.m[3][2]};
            Vec3 target{eye.x + d.x, eye.y + d.y, eye.z + d.z};
            Vec3 up = (std::abs(d.y) > 0.99f) ? Vec3{0,0,1} : Vec3{0,1,0};

            float far  = std::max(L->range(), 0.5f);
            float near = std::max(0.1f, far * 0.005f);
            float fov  = 2.0f * std::max(L->outerAngle(), 0.05f);
            // Cap aperture below 180 deg so the perspective matrix stays sane.
            if (fov > 3.10f) fov = 3.10f;

            Mat4 view = Mat4::lookAt(eye, target, up);
            Mat4 proj = Mat4::perspective(fov, 1.0f, near, far);
            Mat4 projView = proj * view;
            bakeTile(shadowTileCount_, projView, L);
            lightShadowSlot_[i] = shadowTileCount_;
            lightShadowSlotCount_[i] = 1;
            shadowTileCount_++;
        }
        else if (L->kind() == LightNode::Kind::Point) {
            // Point light = 6-face cube projection. Each face gets its own
            // atlas tile rendered with perspective(90deg, 1, near, far).
            // Needs 6 contiguous slots; skip if the budget can't fit them.
            if (shadowTileCount_ + 6 > kMaxShadowTiles) continue;

            const Mat4& M = L->worldMatrix();
            Vec3 eye{M.m[3][0], M.m[3][1], M.m[3][2]};
            float far  = std::max(L->range(), 0.5f);
            float near = std::max(0.05f, far * 0.005f);
            // PI/2 + small fudge so the 6 frusta have a smidge of overlap
            // at the seams; eliminates a single-texel sliver of "no shadow"
            // at face boundaries.
            Mat4 proj = Mat4::perspective(1.5708f, 1.0f, near, far);

            // Cube-face conventions (matches D3D / OpenGL cube map order).
            // Each entry is { forward.xyz, up.xyz }.
            const Vec3 forward[6] = {
                { 1, 0, 0}, {-1, 0, 0},
                { 0, 1, 0}, { 0,-1, 0},
                { 0, 0, 1}, { 0, 0,-1},
            };
            const Vec3 upVec[6] = {
                {0,-1, 0}, {0,-1, 0},
                {0, 0, 1}, {0, 0,-1},
                {0,-1, 0}, {0,-1, 0},
            };

            int firstSlot = shadowTileCount_;
            lightShadowSlot_[i] = firstSlot;
            lightShadowSlotCount_[i] = 6;
            for (int f = 0; f < 6; ++f) {
                Vec3 target{eye.x + forward[f].x,
                            eye.y + forward[f].y,
                            eye.z + forward[f].z};
                Mat4 view = Mat4::lookAt(eye, target, upVec[f]);
                Mat4 projView = proj * view;
                bakeTile(firstSlot + f, projView, L);
                shadowTileCount_++;
            }
        }
    }
}

void SceneGraph::renderShadowPass() {
    if (shadowTileCount_ == 0) return;
    ensureShadowPipeline();
    ensureShadowAtlas();
    if (!shadowProgram_ || !shadowAtlasFBO_) return;

    glBindFramebuffer(GL_FRAMEBUFFER, shadowAtlasFBO_);
    glViewport(0, 0, shadowAtlasSize_, shadowAtlasSize_);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Front-face culling reduces self-shadow acne on closed convex meshes
    // because back-faces (relative to the light) carry the depth value used
    // for comparison. Opens up a peter-panning risk on thin geometry — the
    // normal-bias + constant bias compensate.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    // Slope-scaled depth bias shifts stored depth values away from the light
    // proportional to surface slope. This is the big hammer for self-shadow
    // acne — constant/normal bias alone can't cover the full dynamic range
    // of slopes a directional light sees across the scene.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    glUseProgram(shadowProgram_);

    const int tileSize = shadowAtlasSize_ / 4;  // matches gridDim in prepareShadows

    for (int slot = 0; slot < shadowTileCount_; ++slot) {
        int gx = slot % 4;
        int gy = slot / 4;
        glViewport(gx * tileSize, gy * tileSize, tileSize, tileSize);
        // Scissor the clear so previous frames in other tiles aren't wiped.
        // (The full-FBO clear above handles cold start; per-tile work would
        // skip it once we cache static shadows. Not yet.)

        // shadowRenderMatrix_ holds lightProj*lightView in WORLD space.
        // Per-mesh: uMVP = renderMatrix * meshWorldModel.
        Mat4 lightVP;
        std::memcpy(lightVP.data(), shadowRenderMatrix_[slot], sizeof(float) * 16);

        for (auto* mesh : shadowCasters_) {
            Mat4 mvp = lightVP * mesh->worldMatrix();
            glUniformMatrix4fv(shadowUMVP_, 1, GL_FALSE, mvp.data());
            mesh->drawRaw();
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SceneGraph::render() {
    hasMeshContent_ = false;

    // 2D render path (CanvasScene)
    if (canvasScene_) {
        canvasScene_->reset();
        canvasScene_->save();
        canvasScene_->translate(-cameraX_, -cameraY_);
        if (cameraZoom_ != 1.0f) {
            canvasScene_->scale(cameraZoom_, cameraZoom_);
        }
    }

    // Check for 3D content: mesh nodes OR world-anchored Shape/Sprite/Html.
    // Both render into the mesh FBO (depth-tested against each other) so
    // they share the same setup path.
    bool hasMeshNodes = false;
    bool hasBillboardNodes = false;
    bool hasLightIcons = false;
    for (auto& [id, node] : nodes_) {
        if (!node->visible()) continue;
        if (node->type() == SceneNode::Type::Mesh) hasMeshNodes = true;
        else if (node->hasWorldAnchor())           hasBillboardNodes = true;
        else if (showLightIcons_ && node->type() == SceneNode::Type::Light) hasLightIcons = true;
        if (hasMeshNodes && hasBillboardNodes && hasLightIcons) break;
    }

    // Resolve the gizmo overlay up-front so it can force the 3D pass even
    // when the canvas has no other 3D content. Cached and replayed below.
    std::vector<MeshNode*> gizmoMeshes;
    if (gizmoProvider_) gizmoMeshes = gizmoProvider_(this);
    const bool hasGizmo = !gizmoMeshes.empty();

    const bool has3D = (hasMeshNodes || hasBillboardNodes || hasGizmo || hasLightIcons)
                       && canvasWidth_ > 0 && canvasHeight_ > 0;

    if (has3D) {
        ensureMeshPipeline();
        if (hasBillboardNodes || hasLightIcons) ensureBillboardPipeline();
        ensureMeshFBO();

        // Collect lights once per frame — reused for shadow + mesh + gizmo
        // passes. Done before any FBO bind so the shadow pass can manage
        // its own FBO state cleanly.
        std::vector<LightNode*> lights;
        collectLights(lights);
        static LightNode implicitSun;
        implicitSun.setKind(LightNode::Kind::Directional);
        implicitSun.setDirection(Vec3(-0.3f, -1.0f, -0.5f).normalized());
        implicitSun.setColor(1.0f, 0.98f, 0.95f);
        implicitSun.setIntensity(3.0f);
        std::vector<LightNode*> fallback;
        if (lights.empty()) { fallback.push_back(&implicitSun); }
        const auto& activeLights = lights.empty() ? fallback : lights;

        // Shadow caster pass renders into the shadow atlas (its own FBO).
        // Returns with FBO unbound; the mesh pass below rebinds meshFBO_.
        prepareShadows(activeLights);
        renderShadowPass();

        if (meshFBO_) {
            glBindFramebuffer(GL_FRAMEBUFFER, meshFBO_);
            glViewport(0, 0, meshFBOWidth_, meshFBOHeight_);
            // Reset state that Ganesh may have changed.
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);

            hasMeshContent_ = true;

            // Skybox first — paints the IBL cubemap into the cleared FBO so
            // subsequent geometry naturally composits over it. No-op when no
            // environment is loaded; depth state is left as the geometry
            // pass expects (test on, write on, LESS).
            renderSkyboxPass();

            // --- Mesh pass --------------------------------------------------
            // Lit meshes render to the HDR FBO (pass through tonemap). Unlit
            // meshes are deferred to a post-tonemap overlay pass so their
            // authored colors aren't desaturated by ACES.
            std::vector<MeshNode*> unlitMeshes;
            if (hasMeshNodes && meshProgram_) {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glUseProgram(meshProgram_);

                glUniform1f(uFogStart_, fogStart_);
                glUniform1f(uFogEnd_, fogEnd_);
                glUniform3f(uFogColor_, fogColor_[0], fogColor_[1], fogColor_[2]);
                glUniform3f(uAmbient_, ambientColor_[0], ambientColor_[1], ambientColor_[2]);
                uploadLights(activeLights);

                std::function<void(SceneNode*)> walkMesh = [&](SceneNode* n) {
                    if (!n->visible()) return;
                    if (n->type() == SceneNode::Type::Mesh) {
                        auto* m = static_cast<MeshNode*>(n);
                        if (m->unlit()) unlitMeshes.push_back(m);
                        else            renderMeshNode(m);
                    }
                    for (auto* c : n->children()) walkMesh(c);
                };
                walkMesh(root_.get());

                glDisable(GL_CULL_FACE);
            }

            // --- Billboard pass --------------------------------------------
            // Depth test on (occluded behind geometry), depth write off (so
            // multiple billboards don't occlude each other).
            if ((hasBillboardNodes || hasLightIcons) && bbProgram_) {
                glUseProgram(bbProgram_);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                // Premultiplied "over" — matches both our SDF/rect fills (we
                // premultiply in the fragment shader) and Skia textures (which
                // produce premultiplied output).
                glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                // View-rotation-only matrix: in camera-relative space the eye
                // is at origin, so only orientation remains.
                Mat4 viewRot = viewMatrix_;
                viewRot.m[3][0] = 0.0f;
                viewRot.m[3][1] = 0.0f;
                viewRot.m[3][2] = 0.0f;
                Mat4 vp = projectionMatrix_ * viewRot;
                glUniformMatrix4fv(bbUVP_, 1, GL_FALSE, vp.data());
                glBindVertexArray(bbVAO_);

                std::function<void(SceneNode*)> walkBB = [&](SceneNode* n) {
                    if (!n->visible()) return;
                    if (n->hasWorldAnchor()) {
                        renderBillboardNode(n);
                    }
                    for (auto* c : n->children()) walkBB(c);
                };
                walkBB(root_.get());

                // Light marker icons (editor affordance). Drawn in the same
                // pass so they occlude correctly against geometry.
                if (hasLightIcons) {
                    for (auto& [id, node] : nodes_) {
                        if (!node->visible()) continue;
                        if (node->type() != SceneNode::Type::Light) continue;
                        renderLightIcon(static_cast<LightNode*>(node.get()));
                    }
                }

                glBindVertexArray(0);
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }

            glUseProgram(0);
            glDisable(GL_DEPTH_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // --- Tonemap pass ----------------------------------------------
            // HDR mesh FBO -> LDR output texture. The compositor reads the
            // LDR texture; the HDR texture is internal.
            runTonemapPass();

            // --- Post-tonemap unlit overlay --------------------------------
            // Unlit meshes (scene-editor axes, engine gizmo) render directly
            // into the LDR tonemap target so their authored colors aren't
            // desaturated by ACES. Shares the mesh FBO's depth buffer so they
            // still occlude against scene geometry. Gizmo handles disable
            // depth test to stay always-on-top.
            const bool hasOverlay = !unlitMeshes.empty() || hasGizmo;
            if (hasOverlay && tonemapFBO_ && meshProgram_) {
                glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);
                glViewport(0, 0, tonemapFBOWidth_, tonemapFBOHeight_);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_FALSE);                  // scene depth stays intact
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glUseProgram(meshProgram_);
                glUniform1f(uFogStart_, 0.0f);
                glUniform1f(uFogEnd_, 0.0f);
                glUniform3f(uFogColor_, 0.0f, 0.0f, 0.0f);
                glUniform3f(uAmbient_, 0.0f, 0.0f, 0.0f);
                // uUnlit is set per-mesh by renderMeshNode; still need light
                // uniforms uploaded (shader accesses count even if unused).
                uploadLights(activeLights);

                for (MeshNode* mn : unlitMeshes) {
                    renderMeshNode(mn);
                }

                if (hasGizmo) {
                    glDisable(GL_DEPTH_TEST);
                    for (MeshNode* mn : gizmoMeshes) {
                        if (!mn) continue;
                        renderMeshNode(mn);
                    }
                    glEnable(GL_DEPTH_TEST);
                    hasMeshContent_ = true;
                }

                glDisable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                glDisable(GL_DEPTH_TEST);
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        }
    }

    // --- 2D canvas pass ---------------------------------------------------
    // Render non-world-anchored Shape/Sprite into the 2D overlay layer.
    // World-anchored nodes already rendered into the FBO above.
    {
        std::function<void(SceneNode*)> walk2D = [&](SceneNode* n) {
            if (!n->visible()) return;
            if (n->type() != SceneNode::Type::Mesh &&
                n->type() != SceneNode::Type::Html &&
                !n->hasWorldAnchor()) {
                n->onRender(*this);
            }
            for (auto* c : n->children()) walk2D(c);
        };
        walk2D(root_.get());
    }

    // Restore 2D camera
    if (canvasScene_) {
        canvasScene_->restore();
    }

    // Notify the DOM element of the current FBO texture for compositing.
    // We hand over the tonemapped LDR texture; if tonemap hasn't run (no
    // 3D content this frame) we pass 0 to clear.
    if (fboTexCb_) {
        fboTexCb_(hasMeshContent_ ? tonemapColorTex_ : 0);
    }
}

bool SceneGraph::unprojectLocal(float localX, float localY,
                                Vec3& outOrigin, Vec3& outDir) const {
    if (canvasWidth_ <= 0 || canvasHeight_ <= 0) return false;
    const auto& P = projectionMatrix_;
    const auto& V = viewMatrix_;
    float m11 = P.m[1][1];
    if (!std::isfinite(m11) || m11 <= 0.0f) return false;
    float tanHalfFov = 1.0f / m11;
    // View rows (camera basis in world space).
    // Row 0 = right, row 1 = up, row 2 = -forward (camera looks along -Z).
    Vec3 right  (V.m[0][0], V.m[1][0], V.m[2][0]);
    Vec3 up     (V.m[0][1], V.m[1][1], V.m[2][1]);
    Vec3 forward(-V.m[0][2], -V.m[1][2], -V.m[2][2]);

    float aspect = static_cast<float>(canvasWidth_) / static_cast<float>(canvasHeight_);
    float nx = (2.0f * localX / static_cast<float>(canvasWidth_)) - 1.0f;
    float ny = 1.0f - (2.0f * localY / static_cast<float>(canvasHeight_));

    Vec3 dir = forward
             + right * (nx * aspect * tanHalfFov)
             + up    * (ny * tanHalfFov);
    outDir    = dir.normalized();
    outOrigin = cameraEye_;
    return true;
}

void SceneGraph::renderNode(SceneNode* /*node*/) {
    // Retained for ABI stability but unused now that render() performs
    // explicit mesh / billboard / 2D passes.
}

void SceneGraph::renderMeshNode(MeshNode* mesh) {
    // Camera-relative rendering: offset model position by camera to avoid
    // float precision issues at large world coordinates (planet scale).
    Mat4 model = mesh->worldMatrix();
    model.m[3][0] -= cameraEye_.x;
    model.m[3][1] -= cameraEye_.y;
    model.m[3][2] -= cameraEye_.z;

    // View matrix without translation (rotation only) since model is now
    // camera-relative
    Mat4 viewRot = viewMatrix_;
    viewRot.m[3][0] = 0.0f;
    viewRot.m[3][1] = 0.0f;
    viewRot.m[3][2] = 0.0f;

    Mat4 mvp = projectionMatrix_ * viewRot * model;

    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.data());
    glUniform4fv(uColor_, 1, mesh->color());
    glUniform1f(uEmissive_, mesh->emissive());
    glUniform3fv(uEmissiveColor_, 1, mesh->emissiveColor());
    glUniform1f(uMetallic_, mesh->metallic());
    glUniform1f(uRoughness_, mesh->roughness());
    if (uUnlit_ >= 0) glUniform1i(uUnlit_, mesh->unlit() ? 1 : 0);
    glUniform1i(uUseVertexColor_, mesh->hasVertexColors() ? 1 : 0);
    glUniform1f(uNearClip_, mesh->nearClipDist());

    // Bind baseColor texture if present. Texture wins over vertex-color when
    // both are set — matches glTF PBR "baseColorTexture * baseColorFactor".
    bool bindTex = mesh->hasBaseColorTexture();
    if (bindTex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mesh->baseColorTextureId());
        glUniform1i(uBaseColorTex_, 0);
        glUniform1i(uUseTexture_, 1);
    } else {
        glUniform1i(uUseTexture_, 0);
    }

    // Per-mesh polygon offset (depth bias). Used by callers that need to
    // layer co-located meshes — e.g. terrain LOD shells that overlap and need
    // the high-detail mesh to consistently win the depth test.
    float pf = mesh->depthBiasFactor();
    float pu = mesh->depthBiasUnits();
    if (pf != 0.0f || pu != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pf, pu);
    }

    // Alpha blending for translucent meshes (uniform color alpha < 1).
    // Depth writes are disabled so multiple translucent surfaces don't
    // occlude each other; opaque meshes still occlude translucent ones via
    // the unchanged depth test. Separate alpha function ensures the final
    // FBO stays composable over the 2D backdrop (standard "over" operator).
    bool translucent = mesh->color()[3] < 1.0f;
    if (translucent) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE,       GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    }

    mesh->onRender(*this);

    if (translucent) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    if (pf != 0.0f || pu != 0.0f) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Billboard rendering
// ---------------------------------------------------------------------------

namespace {

// Resolve world-space half-extents for a billboard node.
// ShapeNode uses width/height (or radius for circles), SpriteNode uses
// width/height, HtmlNode converts its pixel layout size by pxPerUnit. All
// defaults keep the quad a reasonable size even if the user forgot to set
// dimensions.
struct BillboardDraw {
    int shapeMode = 0;        // 0 rect, 1 circle, 2 textured
    float halfW = 0.5f;
    float halfH = 0.5f;
    float color[4] = {1, 1, 1, 1};
    float stroke[4] = {0, 0, 0, 0};
    float strokeWidth = 0.0f;
    GLuint texture = 0;
};

static inline void color8(float* out, const Color& c) {
    out[0] = c.r / 255.0f;
    out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f;
    out[3] = c.a / 255.0f;
}

static bool resolveBillboard(SceneNode* node, BillboardDraw& d) {
    using T = SceneNode::Type;
    switch (node->type()) {
    case T::Shape: {
        auto* s = static_cast<ShapeNode*>(node);
        const Vec3& scl = node->scale();
        switch (s->shape()) {
        case ShapeNode::Shape::Rect:
        case ShapeNode::Shape::RoundRect:
            d.shapeMode = 0;
            d.halfW = 0.5f * s->width()  * scl.x;
            d.halfH = 0.5f * s->height() * scl.y;
            break;
        case ShapeNode::Shape::Circle:
            d.shapeMode = 1;
            d.halfW = s->radius() * scl.x;
            d.halfH = s->radius() * scl.y;
            break;
        case ShapeNode::Shape::Ellipse:
            d.shapeMode = 1;
            d.halfW = s->radiusX() * scl.x;
            d.halfH = s->radiusY() * scl.y;
            break;
        default:
            // Polygon / line are 2D-only for world-anchored billboards; fall
            // back to a solid rect bounded by width/height.
            d.shapeMode = 0;
            d.halfW = 0.5f * s->width()  * scl.x;
            d.halfH = 0.5f * s->height() * scl.y;
            break;
        }
        color8(d.color,  s->fillColor());
        if (!s->hasFill()) d.color[3] = 0.0f;
        color8(d.stroke, s->strokeColor());
        // Map stroke width from world units to UV space (0..1 per half-size).
        float uvRef = std::max(d.halfW, d.halfH) * 2.0f;
        d.strokeWidth = (s->hasStroke() && uvRef > 0.0f)
                      ? (s->strokeWidth() / uvRef)
                      : 0.0f;
        return true;
    }
    case T::Sprite: {
        auto* s = static_cast<SpriteNode*>(node);
        const Vec3& scl = node->scale();
        d.shapeMode = 2;
        d.halfW = 0.5f * s->width()  * scl.x;
        d.halfH = 0.5f * s->height() * scl.y;
        d.color[0] = d.color[1] = d.color[2] = 1.0f;
        d.color[3] = s->opacity();
        // Texture upload for SpriteNode billboards is a deliberate follow-up;
        // without a texture the shader has nothing to sample, so fade out.
        d.texture = 0;
        d.color[3] = 0.0f;
        return true;
    }
    case T::Html: {
        auto* h = static_cast<HtmlNode*>(node);
        const Vec3& scl = node->scale();
        float ppu = h->pxPerUnit();
        if (ppu <= 0.0f) ppu = 100.0f;
        d.shapeMode = 2;
        d.halfW = 0.5f * (h->layoutWidth()  / ppu) * scl.x;
        d.halfH = 0.5f * (h->layoutHeight() / ppu) * scl.y;
        d.color[0] = d.color[1] = d.color[2] = 1.0f;
        d.color[3] = 1.0f;
        d.texture = h->textureId();
        if (d.texture == 0) d.color[3] = 0.0f;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

void SceneGraph::renderBillboardNode(SceneNode* node) {
    BillboardDraw d;
    if (!resolveBillboard(node, d)) return;
    if (d.color[3] <= 0.0f && d.shapeMode != 2) return; // invisible shape

    // Anchor in camera-relative space (same precision trick as mesh path).
    const Vec3 anchor = node->worldAnchor();
    const float ax = anchor.x - cameraEye_.x;
    const float ay = anchor.y - cameraEye_.y;
    const float az = anchor.z - cameraEye_.z;

    // Camera basis in world space from rows of view matrix (see renderMesh).
    const Vec3 camRight   {viewMatrix_.m[0][0], viewMatrix_.m[1][0], viewMatrix_.m[2][0]};
    const Vec3 camUp      {viewMatrix_.m[0][1], viewMatrix_.m[1][1], viewMatrix_.m[2][1]};
    const Vec3 camForward { -viewMatrix_.m[0][2], -viewMatrix_.m[1][2], -viewMatrix_.m[2][2]};

    Vec3 right = camRight;
    Vec3 up    = camUp;

    if (node->billboardMode() == SceneNode::BillboardMode::YLock) {
        // Y-lock degenerates when the camera looks nearly straight up/down —
        // the horizontal right vector collapses. Fall back to full billboard.
        if (std::fabs(camForward.y) < 0.99f) {
            up = {0.0f, 1.0f, 0.0f};
            Vec3 flatRight{camRight.x, 0.0f, camRight.z};
            float len = flatRight.length();
            if (len > 1e-5f) {
                right = flatRight * (1.0f / len);
            }
        }
    }

    glUniform3f(bbUAnchorRel_, ax, ay, az);
    glUniform3f(bbURight_, right.x, right.y, right.z);
    glUniform3f(bbUUp_,    up.x,    up.y,    up.z);
    glUniform2f(bbUHalfSize_, d.halfW, d.halfH);
    glUniform1i(bbUShapeMode_, d.shapeMode);
    glUniform4fv(bbUColor_,  1, d.color);
    glUniform4fv(bbUStroke_, 1, d.stroke);
    glUniform1f(bbUStrokeWidth_, d.strokeWidth);

    if (d.shapeMode == 2) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d.texture);
        glUniform1i(bbUTex_, 0);
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Kind-specific visual tuning so all three light types are visually
// distinguishable when overlapping in screen space.
//   Directional: larger disc with a thick white ring (sun-like).
//   Point:       medium disc with a faint outer ring.
//   Spot:        small disc with a heavy colored ring (cone-ish).
void SceneGraph::renderLightIcon(LightNode* light) {
    if (!light) return;

    const Mat4& M = light->worldMatrix();
    const float ax = M.m[3][0] - cameraEye_.x;
    const float ay = M.m[3][1] - cameraEye_.y;
    const float az = M.m[3][2] - cameraEye_.z;

    // Full-billboard (camera-facing) — icons always face the camera.
    const Vec3 camRight{viewMatrix_.m[0][0], viewMatrix_.m[1][0], viewMatrix_.m[2][0]};
    const Vec3 camUp   {viewMatrix_.m[0][1], viewMatrix_.m[1][1], viewMatrix_.m[2][1]};

    const Vec3& lc = light->color();
    // Keep icon visible even for lights with very dark configured colors.
    const float lum = 0.299f * lc.x + 0.587f * lc.y + 0.114f * lc.z;
    const float lift = lum < 0.2f ? 0.2f : 0.0f;
    float core[4] = { lc.x + lift, lc.y + lift, lc.z + lift, 1.0f };

    float ring[4];
    float half, strokeT;

    switch (light->kind()) {
    case LightNode::Kind::Directional:
        half = 0.30f;
        strokeT = 0.18f;
        ring[0] = ring[1] = ring[2] = 1.0f; ring[3] = 1.0f;
        break;
    case LightNode::Kind::Point:
        half = 0.22f;
        strokeT = 0.12f;
        ring[0] = core[0] * 0.5f;
        ring[1] = core[1] * 0.5f;
        ring[2] = core[2] * 0.5f;
        ring[3] = 1.0f;
        break;
    case LightNode::Kind::Spot:
    default:
        half = 0.22f;
        strokeT = 0.28f;
        ring[0] = std::min(core[0] * 0.8f, 1.0f);
        ring[1] = std::min(core[1] * 0.8f, 1.0f);
        ring[2] = std::min(core[2] * 0.8f, 1.0f);
        ring[3] = 1.0f;
        break;
    }

    glUniform3f(bbUAnchorRel_, ax, ay, az);
    glUniform3f(bbURight_, camRight.x, camRight.y, camRight.z);
    glUniform3f(bbUUp_,    camUp.x,    camUp.y,    camUp.z);
    glUniform2f(bbUHalfSize_, half, half);
    glUniform1i(bbUShapeMode_, 3);  // ringed disc
    glUniform4fv(bbUColor_,  1, core);
    glUniform4fv(bbUStroke_, 1, ring);
    glUniform1f(bbUStrokeWidth_, strokeT);

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// ---------------------------------------------------------------------------
// HtmlNode rasterization — runs on the main/GL thread before scene render
// so layout of the detached Documents stays serialized with JS mutations.
// ---------------------------------------------------------------------------

void SceneGraph::materializeHtmlNodes(render::SkiaRenderer* renderer,
                                      layout::FontManager* fontManager) {
    if (!renderer || !fontManager) return;
    for (auto& [id, node] : nodes_) {
        if (node->type() != SceneNode::Type::Html) continue;
        auto* hn = static_cast<HtmlNode*>(node.get());
        hn->materializePending(renderer, fontManager);
    }
}

bool SceneGraph::hasPendingHtmlWork() const {
    for (auto& [id, node] : nodes_) {
        if (node->type() != SceneNode::Type::Html) continue;
        auto* hn = static_cast<HtmlNode*>(node.get());
        if (hn->isHtmlDirty()) return true;
        // doc_ dirty from imperative DOM mutations via node.root.
        if (hn->document() && hn->document()->isDirty()) return true;
    }
    return false;
}

} // namespace bro::scene
