#version 450
// A volume (scene.createVolume): a medium over the whole view, composited
// over the opaque scene and its sky. Its custom fragment chunk defines
//
//   void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
//                   inout float transmittance, inout float depth)
//
// and marches the pixel's ray: `dir` unit and camera-relative (the eye at
// the origin), up to `sceneDist`, the opaque surface's distance along it
// (1e30 over the sky). It returns `radiance`, the light the medium sends the
// eye along the ray, as it leaves the medium; `transmittance`, the share of
// what lies behind that comes through; and `depth`, the distance its light
// comes from, for the air in front of it. The pixel becomes
//
//   scene * transmittance + airT * radiance + (1 - transmittance) * airL
//
// with airT and airL the air between the eye and `depth` (sceneAir: aerial
// perspective, else fog): the scene behind already carries that air's own
// light, so what the medium hides of it is put back. The volumes draw into
// a target of their own (PassVolume), radiance over transmittance in the
// colour and `depth` beside it, and that is reconstructed over frames
// before it lands on the scene: a chunk may jitter its samples with
// camera.frame.x and the jitter averages out.
//
// The chunk may call sceneToSun(), volumeSunLight(), volumeSkyLight(),
// volumeFootprint() and volumeNearEnd() below, and read the engine's camera
// (camera.eyeWorld: the eye's absolute position) and lighting uniforms.
//
// The medium near the eye may instead be given point by point: a chunk that
// also defines
//
//   float userMediumRange()
//   void userMedium(vec3 pos, vec3 dir, float footprint,
//                   out mat4x3 scatter, out vec4 extinction, out float clear)
//
// has its medium integrated once a render over a grid on the view (froxels:
// a cell of VOLUME_NEAR_TILE pixels a side, VOLUME_NEAR_SLICES slices deep,
// about evenly spaced to a 64th of the range and in proportion to the
// distance beyond), from the eye to userMediumRange() (none at 0), and each
// pixel reads the grid to its opaque depth in place of marching it
// (Wronski 2014; Hillaire 2015, Frostbite): a ray from inside a medium spent
// most of its steps within the first kilometre, a dozen pixels' rays taking
// the same steps through the same metres. userMedium answers at
// camera-relative `pos` on the ray along unit `dir`, at a footprint
// (volumeFootprint, the cell's), as four parts of the cell's rays, each
// with its own `scatter` (a column: the light sent toward the eye a metre,
// radiance over length) and `extinction` (a component: its share lost a
// metre), each part seen through its own medium and the cell's light their
// mean. A medium the cell does not resolve is part cover: a cloud filling
// a third of the cell passes two thirds of the light behind it, where a
// third of its density over the slices passes none, so a part holds all of
// the medium or none of it (a medium that resolves gives all four alike).
// `clear` is how far on along `dir` the medium is provably absent (0 when
// it cannot say): the slices sampled within it are not asked, as a march
// steps over empty space. Each slice is sampled at one point (moved within it each render, the
// grain averaging over renders) and integrated over it in closed form with
// the light constant over the slice. userVolume then marches on from
// volumeNearEnd(), the range (0 for the exposure meter's rays and without a
// near field), and its light is seen through the near medium's.
//
// The frame's first METER_RAYS pixels also march rays from the eye for the
// exposure's incident meter (scene/exposure.h), as its dome takes the light,
// a pair each: one in a cone about the sun, for the share of it the medium
// passes, and one cosine-weighted over the sky's hemisphere, for the share
// of the sky it passes and the light it sends down, so a meter under an
// overcast deck reads the deck's light and not the sun it hides. One ray
// each read a broken sky as all cloud or none, so the exposure jumped by
// stops as a cloud's shadow edge crossed the eye, and disagreed with the
// ground about it, whose cloud shadows are averages over its footprint.
// Its rays stand for the light over the scene the view sees
// (volumeFootprint), so a medium that resolves itself to the footprint
// answers them as the ground's light shows it, by the same march. Spread
// over pixels, no invocation marches more than the two.
const int METER_RAYS = 4;   // SceneFrame::kVolumeMeterRays, exposure.comp's
const float METER_SUN_SPREAD = 0.05;   // the sun cone's radius, radians (about 3 degrees)
const int METER_SCENE_TAPS = 4;        // a side of the grid of the view's depth the meter's footprint reads
const float METER_SKY_SCALE = 2.0e4;   // the scene's scale over the sky, m

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;   // the pixel's ray (volume.vert), not unit
layout(location = 1) flat in int inVolume;
#ifndef VOLUME_NEAR_PASS
layout(location = 0) out vec4 outColor;
// The distance the medium's light comes from (`depth`), which the volumes'
// reconstruction reprojects the pixel by (PassVolume).
layout(location = 1) out float outDepth;
#endif

// The opaque depth (SceneTargets::depthSnapshot), in the material set's slot.
layout(set = 2, binding = 0) uniform sampler2D texSceneDepth;

// The opaque surface's distance along the ray through pixel p, 1e30 over
// the sky.
float volumeSceneDistance(ivec2 p) {
    float d = texelFetch(texSceneDepth, p, 0).r;
    if (d <= 0.0) return 1e30;   // reversed depth: 0 is the cleared far plane (the sky)
    vec2 uv = (vec2(p) + 0.5) / camera.viewport.xy;
    vec4 view = camera.invProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return length(view.xyz / view.w);
}
// The meter's readings, METER_RAYS pairs of slots per volume in draw order,
// one pair a ray: the radiance down along the hemisphere's ray (rgb) and the
// sun's transmittance along the cone's (a), then the transmittance along the
// hemisphere's (x) (PassVolume).
layout(std430, set = 2, binding = 5) writeonly buffer VolumeMeter { vec4 slots[]; } volumeMeter;

// The sun's illuminance at camera-relative `pos` (its light's colour times
// intensity): what the air and the planet's shadow leave of it when it is
// the atmosphere's sun. Black without a sun.
vec3 volumeSunLight(vec3 pos) {
    int i = sceneSunIndex();
    if (i < 0) return vec3(0.0);
    vec3 c = lighting.lights[i].color.rgb * lighting.lights[i].color.a;
    if (SCENE_ATMOSPHERE && i == int(lighting.atm.ground.w)) c *= sceneSunTransmittance(pos);
    return c;
}

// The open sky's irradiance over pi at camera-relative `pos` on a surface
// facing `n` (sceneOpenSkyIrradiance).
vec3 volumeSkyLight(vec3 pos, vec3 n) {
    return sceneOpenSkyIrradiance(pos, n);
}

// The width (m) of what the ray's light stands for at distance t along it,
// which a medium resolves itself to: for the pixel's ray, the pixel's cone
// (the pixel's width on an orthographic camera); for a meter's ray, at
// least the scale of the scene the view sees (the geometric mean of its
// opaque distances at METER_SCENE_TAPS points, as far as METER_SKY_SCALE
// over the sky): the meter stands for the light over what the eye looks
// at, so a cloud the ray meets counts as the cloud cover over that ground
// does, the light its surfaces show, not as the one cloud on its line.
float g_volumeConeAngle = 0.0, g_volumeMinWidth = 0.0;
float volumeFootprint(float t) { return max(t * g_volumeConeAngle, g_volumeMinWidth); }

// While a debug view shows (scene/debug_views.h), a chunk that answers it
// (SCENE_DEBUG(name)) calls sceneDebugOut(rgb): the pixel is that colour,
// opaque, unlit and with no air. A volume that does not answer lets all
// through, so what it covers shows.
vec4 g_sceneDebug = vec4(0.0, 0.0, 0.0, -1.0);
void sceneDebugOut(vec3 rgb) { g_sceneDebug = vec4(rgb, 1.0); }

// Where the ray's march starts: the near field's far end (above).
float g_volumeNear = 0.0;
float volumeNearEnd() { return g_volumeNear; }

const int VOLUME_NEAR_SLICES = 128;   // PassVolume::kNearSlices
// The near grid's slice coordinate (0 at the eye, VOLUME_NEAR_SLICES at the
// range) of distance t, and back: linear to a 64th of the range, so the
// first metres are not one slice, and logarithmic beyond.
float volumeNearSlice(float t, float range) {
    return float(VOLUME_NEAR_SLICES) * log(1.0 + 64.0 * t / range) / log(65.0);
}
float volumeNearDistance(float u, float range) {
    return range / 64.0 * (exp(u / float(VOLUME_NEAR_SLICES) * log(65.0)) - 1.0);
}

#if defined(CUSTOM_MEDIUM) && defined(VOLUME_NEAR_PASS)
// The grid (PassVolume): slice k of a cell holds the light (rgb) and
// transmittance (a) from the eye to the slice's far end, and the distance
// weighted by the opacity over it (its integral; over 1 - transmittance it
// is the mean distance the light comes from).
layout(set = 2, binding = 6, rgba16f) uniform writeonly image3D volumeNearLightOut;
layout(set = 2, binding = 7, r32f) uniform writeonly image3D volumeNearDepthOut;
#elif defined(CUSTOM_MEDIUM)
layout(set = 2, binding = 1) uniform sampler3D volumeNearLight;
layout(set = 2, binding = 2) uniform sampler3D volumeNearDepth;
#endif

//__USER_CHUNK__

#ifdef VOLUME_NEAR_PASS
// One cell's column of the near grid, front to back: the pass draws the
// volume's triangle over a viewport of the grid's cells, so inWorldPos is the
// ray through the cell's centre.
void main() {
#ifdef CUSTOM_MEDIUM
    float range = userMediumRange();
    if (!(range > 0.0)) return;   // no near field: nothing reads the grid
    vec3 dir = normalize(inWorldPos);
    ivec2 cell = ivec2(gl_FragCoord.xy);
    float pixel = 2.0 / (float(imageSize(volumeNearLightOut).y) * abs(camera.proj[1][1]));
    bool perspective = camera.proj[3][3] < 0.5;
    g_volumeConeAngle = perspective ? pixel : 0.0;
    g_volumeMinWidth = perspective ? 0.0 : pixel;
    // Each slice's sample at a fraction of it, per cell and moved on by the
    // golden ratio each render, as the volumes' marches are.
    uint h = uint(cell.x) * 1973u + uint(cell.y) * 9277u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    float jitter = fract(float((h ^ (h >> 15)) & 1023u) / 1024.0 + 0.618034 * camera.frame.x);
    // The four parts' transmittances; the light and depth their means.
    vec3 L = vec3(0.0);
    vec4 T = vec4(1.0);
    float D = 0.0, z0 = 0.0, clearTo = 0.0;
    for (int k = 0; k < VOLUME_NEAR_SLICES; k++) {
        float z1 = volumeNearDistance(float(k + 1), range);
        float z = mix(z0, z1, jitter);
        if (z >= clearTo && max(max(T.x, T.y), max(T.z, T.w)) > 0.002) {
            mat4x3 scatter;
            vec4 sigma;
            float clear;
            userMedium(dir * z, dir, volumeFootprint(z), scatter, sigma, clear);
            clearTo = z + max(clear, 0.0);
            float dz = z1 - z0;
            vec4 st = exp(-sigma * dz);
            // Each part's light over the slice per unit of its scatter.
            vec4 lit = T * mix(vec4(dz), (1.0 - st) / max(sigma, 1e-7), greaterThan(sigma, vec4(1e-7)));
            L += 0.25 * (scatter * lit);
            D += 0.25 * dot(T, 1.0 - st) * z;
            T *= st;
        }
        imageStore(volumeNearLightOut, ivec3(cell, k),
                   vec4(min(L, vec3(SCENE_HDR_MAX)), 0.25 * (T.x + T.y + T.z + T.w)));
        imageStore(volumeNearDepthOut, ivec3(cell, k), vec4(D));
        z0 = z1;
    }
#endif
}
#else
void main() {
    vec3 dir = normalize(inWorldPos);
    float sceneDist = volumeSceneDistance(ivec2(gl_FragCoord.xy));
    // A pixel's width over the distance (perspective), or in metres
    // (orthographic): 2 / (rows * proj[1][1]) either way.
    float pixel = 2.0 / (camera.viewport.y * abs(camera.proj[1][1]));
    bool perspective = camera.proj[3][3] < 0.5;
    g_volumeConeAngle = perspective ? pixel : 0.0;
    g_volumeMinWidth = perspective ? 0.0 : pixel;

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    float depth = 0.0;
#ifdef CUSTOM_VOLUME
    // The near grid to the opaque depth (or the range): texel k holds the
    // medium to slice k's far end, so the slice coordinate's texel is half
    // one less, and within the first slice a share of the first.
    vec4 nearLT = vec4(0.0, 0.0, 0.0, 1.0);
    float nearD = 0.0;
#ifdef CUSTOM_MEDIUM
    float range = max(userMediumRange(), 0.0);
    if (range > 0.0) {
        float u = volumeNearSlice(min(sceneDist, range), range);
        vec3 at = vec3(gl_FragCoord.xy / camera.viewport.xy, (max(u, 1.0) - 0.5) / float(VOLUME_NEAR_SLICES));
        nearLT = textureLod(volumeNearLight, at, 0.0);
        nearD = textureLod(volumeNearDepth, at, 0.0).r;
        if (u < 1.0) {
            nearLT = mix(vec4(0.0, 0.0, 0.0, 1.0), nearLT, u);
            nearD *= u;
        }
        g_volumeNear = range;
    }
#endif
    userVolume(dir, sceneDist, radiance, transmittance, depth);
    // What lies past the range, seen through the near medium; the depth the
    // two's light comes from weighted by what each sends.
    float farShare = nearLT.a * (1.0 - clamp(transmittance, 0.0, 1.0));
    float shares = 1.0 - nearLT.a + farShare;
    depth = shares > 1e-6 ? (nearD + farShare * depth) / shares : depth;
    radiance = nearLT.rgb + nearLT.a * radiance;
    transmittance *= nearLT.a;
    g_volumeNear = 0.0;
    if (sceneDebugView() != 0) {
        bool answered = g_sceneDebug.a > 0.0;
        radiance = answered ? g_sceneDebug.rgb : vec3(0.0);
        transmittance = answered ? 0.0 : 1.0;
        depth = 0.0;
    }
    ivec2 meterPixel = ivec2(gl_FragCoord.xy);
    int slot = 2 * (inVolume * METER_RAYS + meterPixel.x);
    if (meterPixel.y == 0 && meterPixel.x < METER_RAYS && slot + 1 < volumeMeter.slots.length()) {
        vec3 up = sceneAtmosphereOn() ? normalize(atmOrigin(vec3(0.0))) : vec3(0.0, 1.0, 0.0);
        vec3 toSun = sceneToSun();
        vec3 su = normalize(cross(toSun, abs(toSun.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 sv = cross(toSun, su);
        vec3 hu = normalize(cross(up, abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 hv = cross(up, hu);
        float a = 2.39996 * float(meterPixel.x);
        vec3 sunL = vec3(0.0), upL = vec3(0.0);
        float sunT = 1.0, upT = 1.0, d = 0.0;
        float logSum = 0.0;
        int seen = 0;
        for (int j = 0; j < METER_SCENE_TAPS; j++) {
            for (int i = 0; i < METER_SCENE_TAPS; i++) {
                vec2 at = (vec2(i, j) + 0.5) / float(METER_SCENE_TAPS);
                float sd = volumeSceneDistance(ivec2(at * camera.viewport.xy));
                if (sd < 1e29) { logSum += log(max(sd, 1e-3)); seen++; }
            }
        }
        g_volumeConeAngle = METER_SUN_SPREAD;
        g_volumeMinWidth = seen > 0 ? exp(logSum / float(seen)) : METER_SKY_SCALE;
        userVolume(normalize(toSun + METER_SUN_SPREAD * (cos(a) * su + sin(a) * sv)), 1e30, sunL, sunT, d);
        // Cosine-weighted over the hemisphere: a disc's equal areas lifted
        // onto it (Malley).
        float r = sqrt((float(meterPixel.x) + 0.5) / float(METER_RAYS));
        userVolume(r * (cos(a) * hu + sin(a) * hv) + sqrt(max(1.0 - r * r, 0.0)) * up, 1e30, upL, upT, d);
        volumeMeter.slots[slot] = vec4(upL, clamp(sunT, 0.0, 1.0));
        volumeMeter.slots[slot + 1] = vec4(clamp(upT, 0.0, 1.0), 0.0, 0.0, 0.0);
    }
#endif
    transmittance = clamp(transmittance, 0.0, 1.0);
    if (transmittance >= 1.0 && max(radiance.r, max(radiance.g, radiance.b)) <= 0.0) discard;

    vec3 airT = vec3(1.0), airL = vec3(0.0);
    if (depth > 0.0) {
        SceneAir air = sceneAir(dir * depth, depth);
        airT = air.transmittance;
        airL = air.inscatter;
    }
    outColor = vec4(min(radiance * airT + (1.0 - transmittance) * airL, vec3(SCENE_HDR_MAX)), transmittance);
    outDepth = depth;
}
#endif
