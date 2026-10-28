#version 450
// A heap past the march, drawn as itself (vulkan/sky/scene_clouds.cpp;
// sky/clouds_layout.h's CL_SHELL_*): the pixel's view traced through the
// heap alone, its forms as the march's (sky/cloud_shells.glsl's
// clShellForms: the world's lobes, the engine's skin), over the stretch of
// the view within its lobes' outlines as far out as the skin may push them:
// through clear air by how far the field says the surface is, inside in
// pieces from where the view meets it, growing, until little is left.
// Lit once, where the view meets it, as the march lights a face (sky/
// cloud_light.glsl's clIntegrate): the depth toward the sun the forms' over
// the first pieces toward it, the cache's past them; the sky's light about
// the heap; the air before it. Premultiplied over what is drawn, behind the
// opaque surfaces (the depth snapshot), weighed by how much of it is handed
// over (clShellWeight); a box's back faces draw none.

layout(location = 0) in vec3 inPos;
layout(location = 1) flat in int inHeap;
layout(location = 0) out vec4 outColor;

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
layout(set = 2, binding = CL_B_DEPTH) uniform sampler2D cloudDepth;
#include "sky/cloud_shells.glsl"
#include "sky/cloud_light.glsl"
#include "sky/cloud_forms_at.glsl"

const int CL_SHELL_STEPS = 40;          // the most the view is read at
const float CL_SHELL_STEP_PIXELS = 4.0; // the least step through clear air, in pixels,
const float CL_SHELL_STEP_LEAST = 10.0; // or metres
const float CL_SHELL_IN_FIRST = 20.0;   // the first piece inside (m, or two pixels)
const float CL_SHELL_OPAQUE = 0.02;     // what is left of the view that ends it
const int CL_SHELL_SUN_PIECES = 4;      // pieces toward the sun read by the forms
const float CL_SHELL_COVER_WHOLE = 0.9; // a cover within a tenth of the whole is the whole (clouds.frag's)

// The forms at frame point `q` (camera-relative `p`) over footprint `foot`,
// as the march draws a heap (sky/cloud_forms_at.glsl's clFormsAt): the
// heap's own field (its column's, as the heaps' map holds it), its surface
// moved by the engine's skin, the billows' and (where it shows) the
// detail's, by the cache's column's band (the height in the cloud: wisps
// under, cauliflower over) and how much of the skin's move it shows. x the
// extinction, y the field (> 0 in cloud), z the share of the pixel it fills
// there (clFormsAt's cover: across the surface's ramp over the pixel).
vec3 clShellForms(ClShell h, vec3 q, vec3 p, float foot) {
    float in_ = cloudHeapColumnIn(clShellColumn(h, q.xy), q.z);
    float base = clamp(0.5 + in_ / clouds.forms.w, 0.0, 1.0);
    // (Clear by the column alone where even the skin's most out, blurred,
    // does not reach: the field's most there, read without the noises.)
    float shows = cloudSkinAt(foot);
    float wB = min(cloudBlur(foot) + CLOUD_SKIN_BLUR * (1.0 - shows), 1.0);
    float most = base + 0.55 * CLOUD_SKIN * (1.0 - wB) - 0.5 + 0.5 * wB;
    if (most < 0.0) return vec3(0.0, most, 0.0);
    ClAt w = clAt(atmOrigin(p));
    float lod = clLod(w, foot);
    float skinShows = clCacheBase(w, lod).y;
    vec3 pd = cloudDrift(w.p);
    vec4 n = cloudBillows(pd, foot);
    float wD = cloudDetailAt(foot);
    vec4 e = vec4(0.0);
    float inCloud = 0.5;
    if (wD > 0.0) {
        vec4 b = cloudCacheBand(w.xy, min(int(lod), CL_CACHE_LEVELS - 1));
        inCloud = clamp((w.h - b.x) / max(b.y - b.x, 1e-3), 0.0, 1.0);
        e = cloudDetail(pd, foot);
    }
    float moves = mix(skinShows, max(skinShows, smoothstep(0.04, 0.12, inCloud)), wD);
    float slope;
    float skin = cloudSkinOf(n, e, wD, inCloud, shows * moves, slope);
    float ramp = wB + cloudSkinBlur(slope, foot);
    float sigma = cloudSigmaOf(base, skin, wB, foot, ramp);
    float face = cloudFieldOf(base, skin, wB) + 0.5 * wB;
    return vec3(sigma, face, sigma > 0.0 ? mix(clamp(face / ramp, 0.0, 1.0), 1.0, wB) : 0.0);
}

void main() {
    ClShell h = clShell(inHeap);
    float tFrag = length(inPos);
    vec3 rd = inPos / tFrag;
    vec3 o = clShellPoint(h, vec3(0.0)), d = clShellDir(h, rd);
    float weight = clShellWeight(h);
    // (The box's near face only: where the view enters it.)
    {
        vec3 lo, hi;
        clShellBox(h, clShellGrow(h.dist) + 2.0 * clouds.params.y * h.dist, lo, hi);
        vec3 inv = 1.0 / mix(vec3(1e-9), d, greaterThan(abs(d), vec3(1e-9)));
        vec3 ta = (lo - o) * inv, tb = (hi - o) * inv;
        float tNear = max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z));
        if (weight <= 0.0 || (tNear > 0.0 && tFrag > tNear + max(1.0, 1e-4 * tFrag))) discard;
    }
    // The opaque surface's distance along the view (reversed-Z: 0 the sky).
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(cloudDepth, px, 0).r;
    float tMax = 1e30;
    if (depth > 0.0) {
        vec2 ndc = (vec2(px) + 0.5) / camera.viewport.xy * 2.0 - 1.0;
        vec4 v = camera.invProj * vec4(ndc, depth, 1.0);
        tMax = length(v.xyz / v.w);
    }
    // The stretch within the lobes' outlines, the skin's most out past them.
    float edge = clouds.forms.w;
    float grow = clShellGrow(h.dist);
    float tA = 1e30, tB = -1e30;
    for (int l = 0; l < CL_HEAP_LOBES; ++l) {
        vec4 lb = h.lobe[l];
        if (lb.w <= 0.0) continue;
        ClShell g = h;
        g.lobe[l] = vec4(lb.xy, lb.z + grow, lb.w + grow);
        g.base[l] = h.base[l] - grow;
        float t0, t1, e;
        if (!clShellThrough(g, l, o, d, t0, t1, e)) continue;
        tA = min(tA, t0);
        tB = max(tB, t1);
    }
    tA = max(tA, 0.0);
    tB = min(tB, tMax);
    if (tB <= tA) discard;

    // The trace.
    float pixel = clouds.params.y;
    float t = tA, T = 1.0, len = 0.0, tFace = -1.0, tClear = -1.0, fClear = 0.0;
    float cover = 0.0, coverLast = 0.0;
    for (int i = 0; i < CL_SHELL_STEPS && t < tB && T > CL_SHELL_OPAQUE; ++i) {
        float foot = pixel * t;
        vec3 f = clShellForms(h, o + d * t, rd * t, foot);
        if (f.x <= 0.0) {
            // Clear: on by how far the surface is at least (the base rises a
            // unit over an edge's metres, the skin within it).
            tClear = t;
            fClear = f.y;
            t +=max(-f.y * edge * 0.5, max(CL_SHELL_STEP_PIXELS * foot, CL_SHELL_STEP_LEAST));
            len = 0.0;
            continue;
        }
        if (tFace < 0.0) {
            // The face by false position on the field across the step
            // through clear air (lit where the view met it, not as deep as
            // the step went in; a column's bound there, more out than the
            // field, leaves it nearer the step's end).
            if (tClear >= 0.0 && fClear < 0.0) {
                float back = (t - tClear) * clamp(f.y / (f.y - fClear), 0.0, 1.0);
                t -= back;
                len = back;   // (the first piece the stretch to where it was read)
            }
            tFace = t;
        }
        // (The most of the pixel it has filled, as the march's clCover: a
        // view that goes in deep is the pixel's whole, one that grazes a
        // rim the share of the pixel the rim fills.)
        float c = min(f.z / CL_SHELL_COVER_WHOLE, 1.0);
        if (T <= CL_SHELL_OPAQUE && c >= coverLast) c = 1.0;
        coverLast = c;
        cover = max(cover, c);
        if (len <= 0.0) len = max(CL_SHELL_IN_FIRST, 2.0 * foot);
        float dt = min(len, tB - t);
        T *= exp(-f.x * dt);
        t += dt;
        len *= 2.0;
    }
    if (T <= CL_SHELL_OPAQUE && coverLast >= cover) cover = 1.0;
    float alpha = cover * (1.0 - T);
    if (tFace < 0.0 || alpha < 1.0 / 1024.0) discard;

    // Lit where the view met it.
    vec3 s = lighting.atm.sunDir.xyz;
    vec3 E = lighting.atm.sunIlluminance.rgb;
    vec3 p = rd * tFace;
    vec3 q = o + d * tFace, sq = clShellDir(h, s);
    float foot = pixel * tFace;
    float tauSun = 0.0, at = 0.0, piece = 25.0 * max(1.0, foot / 15.0);
    for (int k = 0; k < CL_SHELL_SUN_PIECES; ++k) {
        float mid = at + 0.5 * piece;
        tauSun += clShellForms(h, q + sq * mid, p + s * mid, max(foot, piece)).x * piece;
        at += piece;
        piece *= 2.8;
    }
    ClAt wb = clAt(atmOrigin(p + s * at));
    if (wb.h > 0.0 && wb.h < 1.0) tauSun += clCache(wb, clLod(wb, foot)).y;
    ClAt w = clAt(atmOrigin(p));
    float up = clCache(w, clLod(w, foot)).z;
    // (The sky's light about the heap: its middle's.)
    vec3 lo, hi;
    clShellBox(h, 0.0, lo, hi);
    vec3 mid = h.centre + h.up * (0.5 * (lo.z + hi.z));
    vec3 shc[6];
    mat3 shFrame;
    atmSkyLightSh(mid, shc, shFrame);
    vec3 atUp = normalize(atmOrigin(mid));
    vec3 skyAbove = atmShIrradiance(shc, shFrame, atUp);
    vec3 groundBelow = atmShIrradiance(shc, shFrame, -atUp);
    float nu = dot(rd, s);
    float ph[6];
    for (int k = 0; k < 6; ++k) ph[k] = clPhase(nu, exp2(-float(k)));
    // (As the march's face: lit an optical depth in, its depth toward the
    // sun less as much as the view's slope to it takes off.)
    float sigma = clouds.forms.x;
    vec2 tauL = vec2(max(tauSun - nu, 0.0), up);
    vec3 pa = atmOrigin(p);
    ClMarch m = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
    clIntegrate(m, pa, tFace, -log(max(T, 1e-6)) / sigma, sigma, tauL, sigma * max(120.0, foot), s, nu, ph,
                clouds.params.x, E, skyAbove, groundBelow, smoothstep(0.0, 0.2, dot(s, normalize(pa))));
    vec3 airT, airL;
    sceneAtmosphereAir(p, airT, airL);
    vec3 col = (cover * m.L * airT + alpha * airL) * weight;
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), alpha * weight);
}
