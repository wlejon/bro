#version 450
// Plants (scene_plants.glsl, plants.vert): opaque, so no discard and the
// depth test runs before this. A leaf is lit as its lobe's form more than
// its own face (more so as an element stands for more leaves), its crown
// shading it toward the sun: the light that reaches a point is what is left
// after the path through the crown to the sun (its density), so a crown is
// bright on its sunward lobes and deep inside, and the sky reaches its core
// less. The sun shows through the leaves at its rim from behind, and the
// shadow map is read where that path leaves the crown, so the crown never
// shades itself twice. Built twice, as mesh.frag: with SCENE_INDIRECT_OUTPUT
// for the opaque pass while SSAO is on.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inLeafNormal;
layout(location = 2) in vec3 inCrownNormal;
layout(location = 3) in vec3 inAlbedo;
layout(location = 4) in vec3 inGlowColor;
layout(location = 5) in vec4 inCrown;
layout(location = 6) in vec4 inShade;
layout(location = 7) in vec2 inInfo;
layout(location = 8) in vec2 inLeafUV;   // across, along the leaf

layout(location = 0) out vec4 outColor;
#ifdef SCENE_INDIRECT_OUTPUT
layout(location = 1) out vec4 outIndirect;
#endif

#include "scene_camera.glsl"
#include "scene_lighting.glsl"
#include "scene_mesh_push.glsl"

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

    vec3 nLeaf = normalize(inLeafNormal);
    if (!wood && !gl_FrontFacing) nLeaf = -nLeaf;
    vec3 nForm = normalize(inCrownNormal);
    vec3 N = wood ? nLeaf : normalize(mix(nLeaf, nForm, mix(0.6, 0.9, far)));
    vec3 albedo = inAlbedo;
    if (!wood) {
        // A near leaf's own face: darker toward its stalk, its midrib a
        // pale line, its blade a touch lighter toward the edge (thinner);
        // gone as the element stands for many.
        float along = clamp(inLeafUV.y, 0.0, 1.0), across = abs(inLeafUV.x);
        float tone = mix(0.78, 1.05, smoothstep(0.0, 0.8, along)) * (1.0 + 0.12 * smoothstep(0.15, 0.45, across));
        float rib = (1.0 - smoothstep(0.012, 0.035, across)) * (1.0 - smoothstep(0.75, 0.95, along));
        vec3 face = albedo * tone + rib * 0.35 * albedo;
        albedo = mix(face, albedo, far);
    }

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
        vec3 radii = vec3(inCrown.w, inShade.x, inCrown.w);
        vec3 o = (pos - inCrown.xyz) / radii, l = L / radii;
        float a = dot(l, l), b = dot(o, l), c = dot(o, o) - 1.0;
        float disc = b * b - a * c;
        exitT = disc > 0.0 ? max((-b + sqrt(disc)) / a, 0.0) : 0.0;
        vis = exp(-inShade.w * exitT);
        if ((floatBitsToUint(push.pbrParams.w) & MESH_RECEIVES_SHADOW) != 0u)
            vis *= lightShadow(sun, pos + L * exitT, camDist, 0.0, L, 1.0, -2.0);
    }

    float NL = dot(N, L);
    // Wrapped: a lobe's terminator is soft, its shaded side never black.
    float wrap = clamp((NL + 0.45) / 1.45, 0.0, 1.0);
    vec3 direct = albedo / PI * sunRad * wrap * vis;
    vec3 through = vec3(0.0);
    if (!wood) {
        // From behind, through the leaves at the crown's skin.
        float back = pow(max(dot(-V, L), 0.0), 3.0);
        float skin = 1.0 - inShade.y;
        through = inGlowColor * albedo * sunRad / PI * back * inShade.z * sqrt(vis) * skin * (0.4 + 0.6 * max(-NL, 0.0));
        // A glint off a near leaf's face.
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(nLeaf, H), 0.0), 48.0) * (1.0 - far);
        direct += sunRad * vis * spec * 0.05;
    }

    // The sky about the form, less of it deep in the crown.
    vec3 irr = lighting.ambientColor.rgb;
    if (lighting.iblParams.x > 0.5) {
        irr = texture(texIrradiance, rotateY(N, lighting.iblParams.z)).rgb * lighting.iblParams.y;
        irr += lighting.groundBounce.rgb * (0.5 - 0.5 * N.y);
    }
    float ao = mix(1.0, 0.3, inShade.y) * (0.7 + 0.3 * clamp(0.5 + 0.5 * nForm.y, 0.0, 1.0));
    vec3 indirect = irr * albedo * ao;

    vec3 color = direct + through + indirect;
    SceneAir air = sceneAir(pos, camDist);
    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    outColor = vec4(color, mix(1.0, 0.0, air.fade));
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(indirect, 0.0);
#endif
}
