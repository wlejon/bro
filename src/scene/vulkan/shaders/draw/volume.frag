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
// The chunk may call sceneToSun(), volumeSunLight() and volumeSkyLight()
// below, and read the engine's camera (camera.eyeWorld: the eye's absolute
// position) and lighting uniforms.
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
// Spread over pixels, no invocation marches more than the two.
const int METER_RAYS = 4;   // SceneFrame::kVolumeMeterRays, exposure.comp's
const float METER_SUN_SPREAD = 0.05;   // the sun cone's radius, radians (about 3 degrees)

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;   // the pixel's ray (volume.vert), not unit
layout(location = 1) flat in int inVolume;
layout(location = 0) out vec4 outColor;
// The distance the medium's light comes from (`depth`), which the volumes'
// reconstruction reprojects the pixel by (PassVolume).
layout(location = 1) out float outDepth;

// The opaque depth (SceneTargets::depthSnapshot), in the material set's slot.
layout(set = 2, binding = 0) uniform sampler2D texSceneDepth;
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

// True while userVolume answers one of the meter's rays, so a medium whose
// surfaces light themselves by a model of it (an average over a footprint)
// can answer the meter by the same model, and the eye reads the light the
// ground beside it shows.
bool volumeMeterRay = false;

//__USER_CHUNK__

void main() {
    vec3 dir = normalize(inWorldPos);
    float sceneDist = 1e30;
    float d = texelFetch(texSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d > 0.0) {   // reversed depth: 0 is the cleared far plane (the sky)
        vec2 uv = gl_FragCoord.xy / camera.viewport.xy;
        vec4 view = camera.invProj * vec4(uv * 2.0 - 1.0, d, 1.0);
        sceneDist = length(view.xyz / view.w);
    }

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    float depth = 0.0;
#ifdef CUSTOM_VOLUME
    userVolume(dir, sceneDist, radiance, transmittance, depth);
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
        volumeMeterRay = true;
        userVolume(normalize(toSun + METER_SUN_SPREAD * (cos(a) * su + sin(a) * sv)), 1e30, sunL, sunT, d);
        // Cosine-weighted over the hemisphere: a disc's equal areas lifted
        // onto it (Malley).
        float r = sqrt((float(meterPixel.x) + 0.5) / float(METER_RAYS));
        userVolume(r * (cos(a) * hu + sin(a) * hv) + sqrt(max(1.0 - r * r, 0.0)) * up, 1e30, upL, upT, d);
        volumeMeterRay = false;
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
