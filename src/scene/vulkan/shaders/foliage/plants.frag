#version 450
// Plants (scene_plants.glsl, plants.vert): opaque, so no discard and the
// depth test runs before this. What a fragment is (its colour, the normal it
// is lit by, the light through it) is its world's plant module's
// (plantSurface); the light is the engine's. Its crown shades it toward the
// sun: the light that reaches a point is what is left after the path
// through the crown's envelope to the sun (its species' density), so a
// crown is bright on its sunward side and dark deep inside, and the sky
// reaches its core less. The sun shows through the leaves at its rim from
// behind, and the shadow map is read where that path leaves the crown, so
// the crown never shades itself twice. Built twice, as mesh.frag: with
// SCENE_INDIRECT_OUTPUT for the opaque pass while SSAO is on.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inLeafNormal;
layout(location = 2) in vec3 inCrownNormal;
layout(location = 3) in vec3 inAlbedo;
layout(location = 4) flat in int inSpecies;
layout(location = 5) in vec4 inCrown;
layout(location = 6) in vec2 inShade;    // the crown's height radius, how deep in it
layout(location = 7) in vec2 inInfo;
layout(location = 8) in vec2 inLeafUV;   // across, along the element

layout(location = 0) out vec4 outColor;
// (mesh.frag's: the opaque scope's sub-pixel mark; a plant is a surface
// of its own.)
layout(location = 1) out float outMark;
#include "common/scene_subpixel_mark.glsl"
#ifdef SCENE_INDIRECT_OUTPUT
layout(location = 2) out vec4 outIndirect;
#endif

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"
#include "common/scene_mesh_push.glsl"
#define PLANT_SHADE 1
#include "foliage/scene_plant_records.glsl"
#include "foliage/scene_plant_layer.glsl"

// The first directional light: the sun. -1 for none.
int plantSun() {
    int count = int(lighting.params.x);
    for (int i = 0; i < count && i < SCENE_MAX_LIGHTS; ++i)
        if (int(lighting.lights[i].position.w) == 0) return i;
    return -1;
}

void main() {
    vec3 pos = inWorldPos;
    float camDist = max(length(pos), 1e-4);
    vec3 V = -pos / camDist;
    bool wood = inInfo.y > 0.5;
    float far = inInfo.x;
    // How deep in its lobe (0 skin .. 1 core); a daub standing for many
    // leaves, their mean.
    float depth = mix(inShade.y, 0.35, far);

    vec3 nLeaf = normalize(inLeafNormal);
    if (!wood && !gl_FrontFacing) nLeaf = -nLeaf;
    vec3 nForm = normalize(inCrownNormal);
    PlantSurface surf = plantSurface(PlantFragment(pos, nLeaf, nForm, inAlbedo, inLeafUV, far, depth, wood, inSpecies));
    vec3 N = surf.normal;
    vec3 albedo = surf.albedo;
    float density = plantSpecies(inSpecies).density;

    // The path to the sun through the crown (an ellipsoid about inCrown.xyz).
    int sun = plantSun();
    vec3 L = vec3(0.0, 1.0, 0.0);
    vec3 sunRad = vec3(0.0);
    float vis = 1.0;
    float exitT = 0.0;
    if (sun >= 0) {
        SceneLight light = lighting.lights[sun];
        L = -light.direction.xyz;
        sunRad = light.color.rgb * light.color.a;
        if (SCENE_ATMOSPHERE && sun == int(lighting.atm.ground.w)) sunRad *= sceneSunTransmittance(pos);
        vec3 radii = vec3(inCrown.w, inShade.x, inCrown.w);
        vec3 o = (pos - inCrown.xyz) / radii, l = L / radii;
        float a = dot(l, l), b = dot(o, l), c = dot(o, o) - 1.0;
        float disc = b * b - a * c;
        exitT = disc > 0.0 ? max((-b + sqrt(disc)) / a, 0.0) : 0.0;
        vis = exp(-density * exitT);
        if ((floatBitsToUint(push.pbrParams.w) & MESH_RECEIVES_SHADOW) != 0u) {
            // (And the land's shade, the land's sun's and the probe grid's,
            // for the sun they traced.)
            if (sun == int(lighting.sunDirection.w) - 1) vis *= sceneLandSun(pos);
            if (vis > 0.0) vis *= lightShadow(sun, pos + L * exitT, camDist, 0.0, L, 1.0, -2.0);
        }
    }

    float NL = dot(N, L);
    // Wrapped: a lobe's terminator is soft, its shaded side never black.
    float wrap = clamp((NL + 0.45) / 1.45, 0.0, 1.0);
    vec3 direct = albedo / PI * sunRad * wrap * vis;
    vec3 through = vec3(0.0);
    if (!wood) {
        // From behind, through the leaves at the crown's skin.
        float back = pow(max(dot(-V, L), 0.0), 3.0);
        float skin = 1.0 - depth;
        through = surf.glow * albedo * sunRad / PI * back * sqrt(vis) * skin * (0.4 + 0.6 * max(-NL, 0.0));
        // A glint off a near leaf's face.
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(nLeaf, H), 0.0), 48.0) * (1.0 - far);
        direct += sunRad * vis * spec * 0.05;
    }

    // The sky about the form, less of it deep in the crown. The probes hold
    // the crown's own shade where it spans their spacing (its medium in their
    // world, a probe inside it); the leaves' crowding finer than that is
    // left, and all of it where the crown is smaller than the spacing.
    vec3 irr = sceneAmbientIrradiance(pos, N);
    float spacing = sceneProbeSpacing(pos);
    float resolved = spacing > 0.0 ? clamp(inCrown.w / spacing - 0.5, 0.0, 1.0) : 0.0;
    float ao = mix(1.0, mix(0.3, 0.75, resolved), depth) * (0.7 + 0.3 * clamp(0.5 + 0.5 * nForm.y, 0.0, 1.0));
    vec3 indirect = irr * albedo * ao;

    vec3 color = direct + through + indirect;
    SceneAir air = sceneAir(pos, camDist);
    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    outColor = vec4(min(color, vec3(SCENE_HDR_MAX)), mix(1.0, 0.0, air.fade));
    outMark = subpixelMark(0.0);
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(min(indirect, vec3(SCENE_HDR_MAX)), 0.0);
#endif
}

#include "foliage/plant_module.glsl"
