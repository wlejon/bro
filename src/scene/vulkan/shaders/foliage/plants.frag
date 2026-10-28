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
layout(location = 7) in vec3 inInfo;    // x far, y 1 wood (2 a painted crown), z the outline's detail
layout(location = 8) in vec2 inLeafUV;   // across, along the element; a painted crown's point on its disc

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
    bool painted = inInfo.y > 1.5;
    bool wood = inInfo.y > 0.5 && !painted;
    float far = inInfo.x;
    // How deep in its lobe (0 skin .. 1 core); a daub standing for many
    // leaves, their mean.
    float depth = mix(inShade.y, 0.35, far);

    vec3 nLeaf = normalize(inLeafNormal);
    if (!wood && !painted && !gl_FrontFacing) nLeaf = -nLeaf;
    vec3 nForm = normalize(inCrownNormal);
    if (painted) {
        // A painted crown's dome, from its point on the disc (plants.vert's
        // basis about the crown's middle, its rim's normal as there): smooth
        // across its triangles' spokes, where the vertices' normals creased.
        vec3 toEye = -normalize(inCrown.xyz);
        vec3 right = cross(vec3(0.0, 1.0, 0.0), toEye);
        right = dot(right, right) > 1e-6 ? normalize(right) : vec3(1.0, 0.0, 0.0);
        vec3 up = cross(toEye, right);
        nForm = nLeaf = normalize(PLANT_PAINT_RIM * (inLeafUV.x * right + inLeafUV.y * up) + toEye);
    }
    // A near leaf as a leaf: how many pixels it spans (its detail fades in
    // from 3 to 12 along it, so a canopy at 20 m and on holds its mean and
    // never shimmers), its blade cupped across its midrib and arched along
    // it (the normal bent by the slope of h = c(x/w)^2 - c'(v - 0.5)^2 on
    // the element's frame, from the point's derivatives), and how near the
    // blade's edge a point lies (thinner there, so more light through).
    float leafDetail = 0.0, edge = 0.0;
    if (!wood && !painted) {
        vec2 dux = dFdx(inLeafUV), duy = dFdy(inLeafUV);
        vec3 dpx = dFdx(pos), dpy = dFdy(pos);
        float span = 1.0 / max(max(length(vec2(dux.y, duy.y)), length(vec2(dux.x, duy.x))), 1e-4);
        leafDetail = (1.0 - far) * smoothstep(3.0, 12.0, span);
        if (leafDetail > 0.0) {
            // The blade's half-width at this height (plantLeafPoint's
            // outline at the detail it was drawn at: base, its left side,
            // tip), only where the detail shows: a smaller leaf's fragments
            // skip the module's outline.
            float y = clamp(inLeafUV.y, 0.0, 1.0);
            vec2 lo = vec2(0.0), hi = vec2(0.0, 1.0);
            for (int i = 1; i < PLANT_LEAF_POINTS - 1; i += 2) {
                vec2 p = abs(plantLeafPoint(inSpecies, i, inInfo.z));
                if (p.y <= y) lo = p;
                else { hi = p; break; }
            }
            float w = mix(lo.x, hi.x, clamp((y - lo.y) / max(hi.y - lo.y, 1e-3), 0.0, 1.0));
            float x = clamp(inLeafUV.x / max(w, 1e-3), -1.0, 1.0);
            edge = smoothstep(0.55, 1.0, abs(x)) * leafDetail;
            vec3 r1 = cross(dpy, nLeaf), r2 = cross(nLeaf, dpx);
            vec3 tU = r1 * dux.x + r2 * duy.x, tV = r1 * dux.y + r2 * duy.y;
            float su = inversesqrt(max(dot(tU, tU), 1e-20)), sv = inversesqrt(max(dot(tV, tV), 1e-20));
            vec3 bend = tU * su * (0.7 * x) - tV * sv * (0.5 * (y - 0.5));
            nLeaf = normalize(nLeaf - bend * leafDetail);
        }
    }
    PlantSurface surf = plantSurface(PlantFragment(pos, nLeaf, nForm, inAlbedo, inLeafUV, far, depth, wood, inSpecies, painted));
    vec3 N = surf.normal;
    vec3 albedo = surf.albedo;
    float density = plantSpecies(inSpecies).density;

    // The probe grid here, read once for the land's shade and the sky.
    SceneProbe probe = sceneProbe(pos);
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
            if (sun == int(lighting.sunDirection.w) - 1) vis *= sceneLandSun(pos, probe);
            if (vis > 0.0) vis *= lightShadow(sun, pos + L * exitT, camDist, 0.0, L, 1.0, -2.0);
        }
    }

    float NL = dot(N, L);
    // Wrapped: a lobe's terminator is soft, its shaded side never black.
    float wrap = clamp((NL + 0.45) / 1.45, 0.0, 1.0);
    vec3 direct = albedo / PI * sunRad * wrap * vis;
    // The sky about the form, less of it deep in the crown. The probes hold
    // the crown's own shade where it spans their spacing (its medium in their
    // world, a probe inside it); the leaves' crowding finer than that is
    // left, and all of it where the crown is smaller than the spacing.
    vec3 irr = sceneAmbientIrradiance(pos, probe, N);
    float spacing = probe.spacing;
    float resolved = spacing > 0.0 ? clamp(inCrown.w / spacing - 0.5, 0.0, 1.0) : 0.0;
    float crowd = mix(1.0, mix(0.3, 0.75, resolved), depth);
    float ao = crowd * (0.7 + 0.3 * clamp(0.5 + 0.5 * nForm.y, 0.0, 1.0));
    vec3 indirect = irr * albedo * ao;

    vec3 through = vec3(0.0);
    if (!wood) {
        // From behind, through the leaves at the crown's skin.
        float back = pow(max(dot(-V, L), 0.0), 3.0);
        float skin = 1.0 - depth;
        through = surf.glow * albedo * sunRad / PI * back * sqrt(vis) * skin * (0.4 + 0.6 * max(-NL, 0.0));
        // A near leaf is thin: the light on its far face (the sun as much of
        // it as the crown leaves toward it, the sky as much as the crowding
        // above leaves) comes through it diffusely, its transmittance the
        // species' glow on its colour, more of it at the thinner edge. The
        // daub's mean is the crown term above; this is a leaf's own.
        if (leafDetail > 0.0) {
            float sunBack = max(-dot(nLeaf, L), 0.0);
            vec3 backIrr = sunRad * vis * sunBack + sceneAmbientIrradiance(pos, probe, -nLeaf) * crowd;
            through += surf.glow * albedo * backIrr / PI * (1.0 + 0.6 * edge) * leafDetail;
        }
        // The waxy face's sheen: a broad lobe of the sun and the sky's
        // reflection by Fresnel (the cuticle's 0.04), on the face toward the
        // eye; a leaf of a few pixels (and a daub of many) is its mean, as
        // the transmission's detail above.
        float NV = max(dot(nLeaf, V), 0.0);
        // (Schlick's, its grazing rise capped by the cuticle's roughness:
        // a rough face never mirrors the sky.)
        float F = 0.04 + 0.16 * pow(1.0 - NV, 5.0);
        vec3 H = normalize(L + V);
        float NHl = max(dot(nLeaf, H), 0.0), NLl = max(dot(nLeaf, L), 0.0);
        float sheen = distributionGGX(NHl, 0.45) * geometrySmith(NV, NLl, 0.45) * F / max(4.0 * NV, 1e-3);
        float nearW = leafDetail;
        direct += sunRad * vis * sheen * nearW;
        indirect += sceneAmbientIrradiance(pos, probe, reflect(-V, nLeaf)) / PI * F * crowd * nearW;
    }

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
