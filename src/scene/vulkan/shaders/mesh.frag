#version 450
// Every scene mesh: the material inputs (base colour, vertex colour, maps,
// the instance tint and atlas cell), then the shared lighting
// (scene_lighting.glsl), the air (aerial perspective or fog) and the tile
// shade map. Unlit meshes skip the
// lighting. inWorldPos is camera-relative (scene_camera.glsl): the eye is the
// origin. Built twice: as is, and with SCENE_INDIRECT_OUTPUT for the opaque
// pass while SSAO is on; and both again with SCENE_VEGETATION for plants
// (scene_vegetation.glsl): a leaf's normal leans toward its crown's bulk
// normal so the crown lights as one form, its underside reads paler, the
// sun glows through it from behind, and the crown's depth occludes the sky.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;      // vertex colour, white when the mode is off
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;
layout(location = 6) in vec4 inInstColor;  // rgb tint, a atlas cell / 256 (white off instancing)
#ifdef SCENE_VEGETATION
// xyz the crown's bulk normal times its weight, w the ambient occlusion,
// negative on a leaf (vegetationSway), -(2 + ao) on a grass blade.
layout(location = 7) in vec4 inVegShade;
#endif

layout(location = 0) out vec4 outColor;
#ifdef SCENE_INDIRECT_OUTPUT
// The indirect (ambient + probe) light in outColor, for the SSAO pass to
// darken after the opaque pass (scene/vulkan/pass_ssao.h).
layout(location = 1) out vec4 outIndirect;
#endif

#include "scene_camera.glsl"
#include "scene_lighting.glsl"
#include "scene_mesh_push.glsl"
#ifdef SCENE_VEGETATION
#include "scene_forest_sprite.glsl"
#endif

layout(set = 2, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 1) uniform sampler2D texNormal;
layout(set = 2, binding = 2) uniform sampler2D texMetallicRoughness;   // glTF: G roughness, B metallic
layout(set = 2, binding = 3) uniform sampler2D texEmissive;
layout(set = 2, binding = 4) uniform sampler2D texOcclusion;           // R

// The base colour uv: an instance's cell of the atlas grid, else the mesh uv.
// Only the base colour samples the atlas.
vec2 albedoUV() {
    float grid = push.extra.w;
    int cols = max(int(mod(grid, 256.0)), 1);
    int rows = max(int(grid / 256.0), 1);
    if (cols == 1 && rows == 1) return inUV;
    int cell = clamp(int(inInstColor.a * 256.0), 0, cols * rows - 1);
    vec2 cellSize = vec2(1.0 / float(cols), 1.0 / float(rows));
    return (vec2(float(cell % cols), float(cell / cols)) + fract(inUV)) * cellSize;
}

// Custom-shader splice point: a user chunk defining
// `void userFragment(inout vec3 baseColor, inout vec3 normal,
//                    inout float metallic, inout float roughness,
//                    inout vec3 emissive, inout float alpha)` replaces this
// marker and defines CUSTOM_FRAGMENT. It runs after every material input is
// gathered and before the lights, so standard lighting applies to its result.
//__USER_CHUNK__

void main() {
    uint flags = meshFlags();
    float camDist = length(inWorldPos);
    if (push.extra.x > 0.0 && camDist < push.extra.x) discard;
#ifdef SCENE_VEGETATION
    // A forest tree crossing between tiers (ForestNode::fadeCode): a pixel
    // dither h, each tier drawing where h is in its [lo, hi), the tiers of a
    // tree splitting [0, 1) between them, so they cover each pixel once.
    // The code is 2 * round(lo * 4096) + hi; 1 is solid.
    if ((flags & MESH_FOREST) != 0u) {
        float h = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float code = inInstColor.a;
        float k = floor(code * 0.5);
        if (h < k * (1.0 / 4096.0) || h >= code - 2.0 * k) discard;
    }
    bool forestSprite = (flags & MESH_FOREST_SPRITE) != 0u;
    if (forestSprite && !forestSpriteCovers(inUV, inColor.x)) discard;
#endif

    // Base colour: the texture composes with the colour and the vertex tint
    // (white when off); without one the vertex colour replaces or tints it.
    vec3 baseColor;
    float alpha;
    uint vertexColor = meshVertexColorMode();
    if ((flags & MESH_ALBEDO_MAP) != 0u) {
        vec4 tex = texture(texAlbedo, albedoUV());
        baseColor = tex.rgb * push.baseColor.rgb * inColor.rgb;
        alpha = tex.a * push.baseColor.a * inColor.a;
    } else if (vertexColor == 1u) {
        baseColor = inColor.rgb;
        alpha = inColor.a;
    } else if (vertexColor == 2u) {
        baseColor = push.baseColor.rgb * inColor.rgb;
        alpha = push.baseColor.a * inColor.a;
    } else {
        baseColor = push.baseColor.rgb;
        alpha = push.baseColor.a;
    }
    baseColor *= inInstColor.rgb;
    if (push.pbrParams.z > 0.0 && alpha < push.pbrParams.z) discard;
#ifdef SCENE_VEGETATION
    // A grass blade (mesh_grass.vert) marks itself with w <= -2: w = -(2 +
    // ao + 2k), k (0..15) how far its translucency has faded with distance.
    // Translucent like a leaf, with no paler underside.
    bool vegLeaf = inVegShade.w < 0.0;
    bool vegBlade = inVegShade.w < -1.5;
    float vegBladeT = -inVegShade.w - 2.0;
    float vegFade = vegBlade ? floor(vegBladeT * 0.5) : 0.0;
    float vegAO = vegBlade ? vegBladeT - 2.0 * vegFade : abs(inVegShade.w);
    // A leaf's underside is paler and yellower than its top.
    if (vegLeaf && !vegBlade && !forestSprite && !gl_FrontFacing)
        baseColor = mix(baseColor, baseColor * vec3(1.25, 1.3, 1.05) + vec3(0.015, 0.02, 0.0), 0.6);
    // A sprite stands in for many leaves: in a gust (inColor.y, the drag at
    // its tree) some turn their undersides up, so the crown lightens as the
    // gust passes over it.
    if (forestSprite) {
        float sheen = clamp((inColor.y - 0.12) * 1.6, 0.0, 1.0);
        baseColor = mix(baseColor, baseColor * vec3(1.25, 1.3, 1.05) + vec3(0.015, 0.02, 0.0), 0.4 * sheen);
        // A clump (inColor.z 1) shows its leaves; a crown is too small on
        // screen for them.
        if (inColor.z > 0.5) baseColor *= forestSpriteLeafShade(inUV, inColor.x);
    }
#endif

    vec3 emissive = push.emissive.rgb * push.emissive.a;
    vec3 geomN = normalize(inNormal);
    SceneAir air = sceneAir(inWorldPos, camDist);

    // Unlit is the authored base colour as is: no lights and no emissive
    // (GL's unlit branch), only the air in front of it.
    if ((flags & MESH_UNLIT) != 0u) {
        vec3 color = baseColor * air.transmittance + air.inscatter;
        if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) color *= cellShade(inWorldPos, geomN);
        // Unlit surfaces reflect nothing in the SSR mask phase.
        outColor = vec4(color, (flags & MESH_REFLECTANCE) != 0u ? 0.0 : mix(alpha, 0.0, air.fade));
#ifdef SCENE_INDIRECT_OUTPUT
        outIndirect = vec4(0.0);
#endif
        return;
    }

    // Two-sided thin surfaces shade the face the camera sees.
    vec3 N = geomN;
    if ((flags & MESH_TWO_SIDED) != 0u && !gl_FrontFacing) N = -N;
    if ((flags & (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) == (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) {
        vec3 nTS = texture(texNormal, inUV).xyz * 2.0 - 1.0;
        N = normalize(mat3(normalize(inTangent), normalize(inBitangent), N) * nTS);
    }
    vec3 shadePos = inWorldPos;
#ifdef SCENE_VEGETATION
    // A sprite is lit as the sphere of leaves it stands in for, at the
    // sphere's surface (its tangent's length is the radius): the flat disc
    // would cut the light-facing disc it casts with in a hard line.
    if (forestSprite) {
        float r2 = min(dot(inUV, inUV), 1.0);
        float rise = sqrt(1.0 - r2);
        N = normalize(normalize(inTangent) * inUV.x + normalize(inBitangent) * inUV.y + geomN * rise);
        shadePos = inWorldPos + geomN * (rise * length(inTangent));
    }
    // The crown as one form: the leaf's own normal blended toward the bulk.
    float bulkWeight = length(inVegShade.xyz);
    if (bulkWeight > 1e-4) N = normalize(N * (1.0 - bulkWeight) + inVegShade.xyz);
#endif

    float metallic = push.pbrParams.x;
    float roughness = push.pbrParams.y;
    if ((flags & MESH_METALLIC_ROUGHNESS) != 0u) {
        vec4 mr = texture(texMetallicRoughness, inUV);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if ((flags & MESH_EMISSIVE_MAP) != 0u) emissive *= texture(texEmissive, inUV).rgb;

#ifdef CUSTOM_FRAGMENT
    userFragment(baseColor, N, metallic, roughness, emissive, alpha);
    N = normalize(N);
#endif

    SceneSurface s;
    s.position = shadePos;
    s.normal = N;
    s.view = normalize(-inWorldPos);
    s.camDist = camDist;
    s.baseColor = baseColor;
    s.metallic = clamp(metallic, 0.0, 1.0);
    s.roughness = clamp(roughness, 0.04, 1.0);
    s.receivesShadow = (flags & MESH_RECEIVES_SHADOW) != 0u;
    s.twoSided = (flags & MESH_TWO_SIDED) != 0u;
    s.subsurface = push.extra.y;
    s.translucency = 0.0;
#ifdef SCENE_VEGETATION
    if (vegLeaf) s.translucency = (push.extra.y > 0.0 ? min(1.4 * push.extra.y, 1.0) : 0.6) * (1.0 - vegFade / 15.0);
#endif

    vec3 indirect = sceneAmbient(s);
    if ((flags & MESH_OCCLUSION_MAP) != 0u) indirect *= texture(texOcclusion, inUV).r;
#ifdef SCENE_VEGETATION
    indirect *= vegAO;
#endif
    vec3 color = sceneDirectLight(s) + indirect + emissive;

    // SSR mask phase: alpha carries the reflectance the SSR pass weights its
    // reflections by (normal-incidence Fresnel luminance times smoothness^2);
    // the SSR pass restores coverage before anything blends against it.
    if ((flags & MESH_REFLECTANCE) != 0u) {
        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
        alpha = dot(F0, vec3(0.2126, 0.7152, 0.0722)) * (1.0 - s.roughness) * (1.0 - s.roughness);
    }

    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    alpha = mix(alpha, 0.0, air.fade);

    if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) {
        float shade = cellShade(inWorldPos, geomN);
        color *= shade;
        indirect *= shade;
    }

    outColor = vec4(color, alpha);
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(indirect, 0.0);
#endif
}
