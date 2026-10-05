#version 450
// Every scene mesh: the material inputs (base colour, vertex colour, maps,
// the instance tint and atlas cell), then the shared lighting
// (scene_lighting.glsl), the air (aerial perspective or fog) and the tile
// shade map. Unlit meshes skip the
// lighting. inWorldPos is camera-relative (scene_camera.glsl): the eye is the
// origin. Built twice: as is, and with SCENE_INDIRECT_OUTPUT for the opaque
// pass while SSAO is on.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;      // vertex colour, white when the mode is off
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;
layout(location = 6) in vec4 inInstColor;  // rgb tint, a atlas cell / 256 (white off instancing)

layout(location = 0) out vec4 outColor;
#ifdef SCENE_INDIRECT_OUTPUT
// The indirect (ambient + probe) light in outColor, for the SSAO pass to
// darken after the opaque pass (scene/vulkan/pass_ssao.h).
layout(location = 1) out vec4 outIndirect;
#endif

#include "scene_camera.glsl"
#include "scene_lighting.glsl"
#include "scene_mesh_push.glsl"

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
    s.position = inWorldPos;
    s.normal = N;
    s.view = normalize(-inWorldPos);
    s.camDist = camDist;
    s.baseColor = baseColor;
    s.metallic = clamp(metallic, 0.0, 1.0);
    s.roughness = clamp(roughness, 0.04, 1.0);
    s.receivesShadow = (flags & MESH_RECEIVES_SHADOW) != 0u;
    s.twoSided = (flags & MESH_TWO_SIDED) != 0u;
    s.subsurface = push.extra.y;

    vec3 indirect = sceneAmbient(s);
    if ((flags & MESH_OCCLUSION_MAP) != 0u) indirect *= texture(texOcclusion, inUV).r;
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
