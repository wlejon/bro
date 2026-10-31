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

// Whether the draw cuts pixels (a near clip, an alpha cutoff): only then are
// its discards built in, as a stage that can discard is depth-tested after it
// shades (SceneMeshDrawer's raster::kCuts).
layout(constant_id = 1) const bool MESH_CUTS = true;
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;      // vertex colour, white when the mode is off
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;
layout(location = 6) in vec4 inInstColor;  // rgb tint, a atlas cell / 256 (white off instancing)
#ifdef SCENE_VEGETATION
// xyz the crown's bulk normal times its weight, w the ambient occlusion,
// negative on a leaf (vegetationSway).
layout(location = 7) in vec4 inVegShade;
#endif

layout(location = 0) out vec4 outColor;
#ifdef SCENE_INDIRECT_OUTPUT
// The indirect (ambient) light in outColor, for the SSAO pass to darken
// after the opaque pass (scene/vulkan/post/pass_ssao.h).
layout(location = 1) out vec4 outIndirect;
#endif

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"
#include "common/scene_mesh_push.glsl"

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
// A chunk may also define `void userLight(vec3 toSun, inout float
// sunVisibility, inout float skyVisibility, inout vec3 skyNormal, inout float
// occluderSun, inout float sunDiffuse)` (it then defines CUSTOM_LIGHT): the
// occlusion it knows beyond the shadow maps and the sun a medium over it
// scatters down (SceneSurface's fields of those names), called after
// userFragment.
//
// While a debug view shows (scene/debug_views.h), a chunk that answers it
// (SCENE_DEBUG(name)) calls sceneDebugOut(rgb): the pixel is that colour,
// unlit, with no air, at an exposure of 1 and a linear tone map. A surface
// that does not answer is a dim grey lit from the eye, for where things are.
vec4 g_sceneDebug = vec4(0.0, 0.0, 0.0, -1.0);
void sceneDebugOut(vec3 rgb) { g_sceneDebug = vec4(rgb, 1.0); }
//__USER_CHUNK__

// A debug view's pixel for a surface facing N (sceneDebugOut's, else the
// grey), when one shows.
bool sceneDebugPixel(vec3 N, vec3 view, out vec3 rgb) {
    if (sceneDebugView() == 0) return false;
    rgb = g_sceneDebug.a > 0.0 ? g_sceneDebug.rgb : vec3(0.06 + 0.2 * abs(dot(N, view)));
    return true;
}

void main() {
    uint flags = meshFlags();
    vec3 worldPos = inWorldPos;
    bool frontFacing = gl_FrontFacing;
    float camDist = length(worldPos);
    if (MESH_CUTS && push.extra.x > 0.0 && camDist < push.extra.x) discard;

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
    if (MESH_CUTS && push.pbrParams.z > 0.0 && alpha < push.pbrParams.z) discard;
#ifdef SCENE_VEGETATION
    bool vegLeaf = inVegShade.w < 0.0;
    float vegAO = abs(inVegShade.w);
    // A leaf's face is the one its normal points to. Its frame (the clump
    // and scatter placements, clumpLeafFrame, bromesh's leaf scatter) is a
    // mirror image (across, normal, tip are left-handed), so its triangles
    // wind the other way and gl_FrontFacing names the wrong face: the eye
    // would see a sunlit top lit as its own underside, and a leaf against
    // the sun lit on the side away from it.
    if (vegLeaf) frontFacing = dot(inNormal, -inWorldPos) >= 0.0;
    // A leaf's underside is paler and yellower than its top.
    vec3 vegUnder = baseColor * vec3(1.25, 1.3, 1.05) + vec3(0.015, 0.02, 0.0);
    if (vegLeaf && !frontFacing) baseColor = mix(baseColor, vegUnder, 0.6);
#endif

    vec3 emissive = push.emissive.rgb * push.emissive.a;
    vec3 geomN = normalize(inNormal);
    SceneAir air = sceneAir(worldPos, camDist);

    // Unlit is the authored base colour as is: no lights and no emissive
    // (GL's unlit branch), only the air in front of it.
    if ((flags & MESH_UNLIT) != 0u) {
        vec3 color = baseColor * air.transmittance + air.inscatter;
        vec3 shown;
        if (sceneDebugPixel(geomN, normalize(-worldPos), shown)) color = shown;
        if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) color *= cellShade(worldPos, geomN);
        // Unlit surfaces reflect nothing in the SSR mask phase.
        outColor = vec4(color, (flags & MESH_REFLECTANCE) != 0u ? 0.0 : mix(alpha, 0.0, air.fade));
#ifdef SCENE_INDIRECT_OUTPUT
        outIndirect = vec4(0.0);
#endif
        return;
    }

    // Two-sided thin surfaces shade the face the camera sees.
    vec3 N = geomN;
    if ((flags & MESH_TWO_SIDED) != 0u && !frontFacing) N = -N;
    if ((flags & (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) == (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) {
        vec3 nTS = texture(texNormal, inUV).xyz * 2.0 - 1.0;
        N = normalize(mat3(normalize(inTangent), normalize(inBitangent), N) * nTS);
    }
#ifdef SCENE_VEGETATION
    // The crown as one form: the leaf's own normal blended toward the bulk.
    {
        float bulkWeight = length(inVegShade.xyz);
        float bulkKeep = 1.0;
        vec3 vegShadeN = inVegShade.xyz;
        if (vegLeaf) {
            // Near, a leaf is its own face, not the crown's: folded along
            // its midrib (the halves tilt apart, so one takes more light than
            // the other) and turned its own way more than the bulk's, so the
            // leaves of a sunlit side differ leaf by leaf rather than lying
            // as one flat sheet. The halves curve away from a midrib that
            // is level (the tilt grows as the square of the way out), so a
            // leaf shades round, not as two flat facets. Gone by the
            // distance a leaf is a few pixels.
            float near = 1.0 - smoothstep(6.0, 25.0, camDist);
            if (near > 0.0) {
                float across = clamp(inUV.x * 2.0 - 1.0, -1.0, 1.0);
                vec3 T = normalize(inTangent);
                N = normalize(N - (frontFacing ? 1.0 : -1.0) * 0.55 * near * across * abs(across) * T);
                bulkKeep = 1.0 - 0.5 * near;
            }
        }
        if (bulkWeight > 1e-4)
            N = normalize(N * (1.0 - bulkWeight * bulkKeep) + vegShadeN * bulkKeep);
    }
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
    s.position = worldPos;
    s.normal = N;
    s.view = normalize(-worldPos);
    s.camDist = camDist;
    s.baseColor = baseColor;
    s.metallic = clamp(metallic, 0.0, 1.0);
    s.roughness = clamp(roughness, 0.04, 1.0);
    s.receivesShadow = (flags & MESH_RECEIVES_SHADOW) != 0u;
    s.twoSided = (flags & MESH_TWO_SIDED) != 0u;
    s.subsurface = push.extra.y;
    s.translucency = 0.0;
#ifdef SCENE_VEGETATION
    if (vegLeaf) s.translucency = push.extra.y > 0.0 ? min(1.4 * push.extra.y, 1.0) : 0.6;
#endif
    s.sunVisibility = 1.0;
    s.skyVisibility = 1.0;
    s.skyNormal = N;
    s.occluderSun = 0.5;
    s.sunDiffuse = 0.0;
#ifdef CUSTOM_LIGHT
    userLight(sceneToSun(), s.sunVisibility, s.skyVisibility, s.skyNormal, s.occluderSun, s.sunDiffuse);
    s.sunVisibility = clamp(s.sunVisibility, 0.0, 1.0);
    s.skyVisibility = clamp(s.skyVisibility, 0.0, 1.0);
    s.skyNormal = normalize(s.skyNormal);
    s.occluderSun = clamp(s.occluderSun, 0.0, 1.0);
    s.sunDiffuse = clamp(s.sunDiffuse, 0.0, 1.0);
#endif

    vec3 indirect = sceneAmbient(s);
    if ((flags & MESH_OCCLUSION_MAP) != 0u) indirect *= texture(texOcclusion, inUV).r;
#ifdef SCENE_VEGETATION
    indirect *= vegAO;
#endif
    vec3 color = sceneDirectLight(s) + indirect + emissive;

    // SSR mask phase: alpha carries the reflectance the SSR pass weights its
    // reflections by: Schlick's Fresnel at the angle the surface is seen at
    // (its luminance), so water and glass seen across mirror what is about
    // them as they do. A rougher surface's reflection is blurred, not
    // dimmer, but the march cannot blur it: the weight fades out from
    // roughness 0.4 to 0.6 and none is marched beyond, where the reflection
    // is a blur the environment's light already gives (and every grazing
    // pixel of rough ground would march). The SSR pass restores coverage
    // before anything blends against it.
    if ((flags & MESH_REFLECTANCE) != 0u) {
        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
        float c = 1.0 - clamp(dot(s.normal, s.view), 0.0, 1.0);
        float c2 = c * c;
        vec3 F = F0 + (1.0 - F0) * (c2 * c2 * c);
        alpha = dot(F, vec3(0.2126, 0.7152, 0.0722)) * (1.0 - smoothstep(0.4, 0.6, s.roughness));
    }

    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    alpha = mix(alpha, 0.0, air.fade);

    if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) {
        float shade = cellShade(worldPos, geomN);
        color *= shade;
        indirect *= shade;
    }

    vec3 shown;
    if (sceneDebugPixel(s.normal, s.view, shown)) {
        color = shown;
        indirect = vec3(0.0);
        if ((flags & MESH_REFLECTANCE) != 0u) alpha = 0.0;
    }

    outColor = vec4(min(color, vec3(SCENE_HDR_MAX)), alpha);
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(min(indirect, vec3(SCENE_HDR_MAX)), 0.0);
#endif
}
