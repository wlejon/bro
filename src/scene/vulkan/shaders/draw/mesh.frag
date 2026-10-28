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
// The cover's two passes in the opaque pass (SceneMeshDrawer::CoverPass): a
// blade's or herb's cut decides its depth alone (MESH_COVER_DEPTH: nothing
// lit, nothing written but the depth), then each visible pixel is lit once
// against that depth (MESH_COVER_SHADE: no cut, so it is depth-tested
// before it shades).
layout(constant_id = 3) const bool MESH_COVER_DEPTH = false;
layout(constant_id = 4) const bool MESH_COVER_SHADE = false;
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;      // vertex colour, white when the mode is off
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;
layout(location = 6) in vec4 inInstColor;  // rgb tint, a atlas cell / 256 (white off instancing)
#ifdef SCENE_VEGETATION
// xyz the crown's bulk normal times its weight, w the ambient occlusion,
// negative on a leaf (vegetationSway); on a grass blade xyz its sward's up
// times its bulk, w -(2 + ao + 2 * its translucency's fade in 15ths).
layout(location = 7) in vec4 inVegShade;
#endif
#ifdef SCENE_HERB
// A herb plant's card (mesh_herb.vert): the world's module's own (its
// HerbVertex id, b, c; inColor its a), w the card's dither seed.
layout(location = 8) flat in vec4 inHerbInfo;
layout(location = 9) in vec4 inHerbSize;
layout(location = 10) in vec2 inHerbStand;
#define HERB_SHADE 1
#endif
#ifdef SCENE_GRASS
// A grass blade (mesh_grass.vert): inColor the world's sward module's own
// (its SwardVertex info), inInstColor.a the herb bloom it carries.
#define SWARD_SHADE 1
#endif

layout(location = 0) out vec4 outColor;
// The opaque scope's sub-pixel mark (common/scene_subpixel_mark.glsl): what
// grows a surface of its own, with the share of it that is a cut card's
// dither on detail too fine to show; anything else the ground it stands on.
// Outside the opaque scope the write goes nowhere.
layout(location = 1) out float outMark;
#include "common/scene_subpixel_mark.glsl"
float meshMark(float subpixel) {
#ifdef SCENE_VEGETATION
    return subpixelMark(subpixel);
#else
    return 0.0;
#endif
}
#ifdef SCENE_INDIRECT_OUTPUT
// The indirect (ambient + probe) light in outColor, for the SSAO pass to
// darken after the opaque pass (scene/vulkan/post/pass_ssao.h).
layout(location = 2) out vec4 outIndirect;
#endif

#ifdef SCENE_VEGETATION
#include "common/scene_cover_mask.glsl"
#endif

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"
#include "common/scene_mesh_push.glsl"
#ifdef SCENE_BARK
#include "draw/scene_bark.glsl"
#endif
#ifdef SCENE_HERB
#include "ground/scene_grass.glsl"
#endif
#ifdef SCENE_GROUND
#include "ground/scene_grass.glsl"
#include "ground/scene_ground.glsl"
#endif
#ifdef SCENE_GRASS
#include "ground/scene_grass.glsl"
#endif
#ifdef SCENE_LAND
#include "ground/scene_land.glsl"
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

#if defined(SCENE_LAND) || defined(SCENE_GROUND)
// A land's canopy at `worldPos` (camera-relative), read a fragment at a
// time from its tiles (set 3's, or the turf's grass node's set 4) at the
// level its rings read there, so a forest's patches past the plants' range
// are the tiles' texels, not the rings' cells: rgb its leaves' albedo, a
// how much of it is shown (scene_land.glsl's landCanopyShown, as the
// rings rise by it, mesh_land.vert's landRise) times its cover along the
// eye's line, its crowns' area and their flanks seen at its slant
// (world/plant_canopy.h): a wood whose crowns stand apart from overhead is
// whole along a low line, as its plants are.
vec4 landCanopyAt(vec3 worldPos) {
    vec3 p = worldPos + camera.eyeWorld.xyz;
    if (!landHasCanopy()) return vec4(0.0);
    float shown = landCanopyShown(length(p - landEye3()), landCanopyRange());
    if (shown <= 0.0) return vec4(0.0);
    vec3 crowns;
    vec2 m = landMapOf(p);
    vec4 c = landSampleCanopy(m, landLevel(m), crowns);
    float down = max(-worldPos.y / max(length(worldPos), 1e-3), 0.02);
    float tau = -log(max(1.0 - c.a, 1e-4)) + crowns.y * sqrt(1.0 - down * down) / down;
    return vec4(c.rgb, (1.0 - exp(-tau)) * shown);
}

// The sun the ground keeps among a land's plants past their cast range at
// `worldPos` (camera-relative): the shadow pages hold none of them there,
// and the land's sun holds their canopy as a sheet (land_sun.comp), whose
// shade reaches only a texel or two past a stand's edge under a high sun,
// while its gaps are finer than its texels. Their crowns as the canopy has
// them (world/plant_canopy.h: tau0 their area a square metre, their flanks
// a square metre, top and base). Ground the eye sees between the crowns:
// no crown stands within a crown's radius r of the eye's line, and the
// sun's line, leaning from it by `apart` a metre climbed, passes the crowns
// from base * apart to top * apart off it. What shades it is the crowns
// the sun's line meets, tau0 plus the flanks it sweeps, less those about
// the eye's line, a lens of two discs base * apart apart (a sun behind the
// eye shades no gap it sees, a low one past a tall trunk shades it as any
// ground). r is the crowns' own: a crown's flank is its radius by its
// depth by CANOPY_FLANK (a cone's 1 to a round crown's pi/2). Ground the
// eye sees through a crown, its leaves' holes (CANOPY_THROUGH of what it
// sees), is shaded as any ground is, by tau0 plus the flanks the sun's
// line sweeps. The shade comes in over the cast range's last tenth as the
// casters go. (Set so the ground is what it is under its plants cast into
// the shadow pages, the same land both ways: the planet's woods from 2 km
// straight down, their leaves unlit, 9.94 stops with the plants cast,
// 10.35 between the crowns alone, 8.87 as any ground.)
const float CANOPY_FLANK = 1.3;
const float CANOPY_THROUGH = 0.4;
float landCanopyGround(vec3 worldPos) {
    if (!landHasCanopy()) return 1.0;
    vec3 p = worldPos + camera.eyeWorld.xyz;
    float past = landCanopyShown(length(p - landEye3()), landCanopyCastRange());
    vec3 L = -lighting.sunDirection.xyz;
    if (past <= 0.0 || lighting.sunDirection.w < 0.5 || L.y <= 0.0) return 1.0;
    vec3 crowns;
    vec2 m = landMapOf(p);
    vec4 c = landSampleCanopy(m, landLevel(m), crowns);
    if (c.a <= 0.0 || crowns.y <= 0.0) return 1.0;
    float tau0 = -log(max(1.0 - c.a, 1e-4));
    float r = CANOPY_FLANK * max(crowns.x - crowns.z, 0.0) * tau0 / (3.14159265 * crowns.y);
    vec3 V = normalize(-worldPos);
    vec2 sunLean = L.xz / L.y;
    float apart = length(sunLean - V.xz / max(V.y, 0.02));
    float q = clamp(crowns.z * apart / max(2.0 * r, 1e-3), 0.0, 1.0);
    float lens = (acos(q) - q * sqrt(1.0 - q * q)) / 1.57079633;
    float any = tau0 + crowns.y * length(sunLean);
    float gap = min(tau0 * (1.0 - lens) + crowns.y * apart, any);
    return mix(1.0, mix(exp(-gap), exp(-any), CANOPY_THROUGH), past);
}
#endif

// A land's canopy (landCanopyAt): its leaves' albedo `albedo`
// shown by `shown` over the surface, as the far plants are drawn (lit as a
// sward, below), facing up whatever the ground's slope, matte, and painted
// a crown at a time as the plants' painted crowns it takes over from are:
// a dome in each CANOPY_CELL m cell, set off its middle by the cell's
// hash, brighter at its top and darker in the gaps between (its tone about
// its own mean, so the canopy's light is what it was), its normal leaning
// out from its middle; while a crown spans a few pixels, its mean beyond.
// Returns what its light is taken down by: crowns in their own shade
// (CANOPY_SHADE: a crown's leaves shade each other, as the trees' far daubs
// do), the more of it seen the lower the eye's line to them (down to
// CANOPY_GRAZING where the line is under a tenth of the vertical, from all
// of it straight down: their shaded sides and the shade between them). The
// shade is of all the light off the leaves, their sheen as well as what
// they scatter (a sward's tops are lit, the crowns' depths are not), and
// of the ground between them in the same share. (Set so the canopy is what
// the plants it stands for are, the same land drawn both ways, the
// planet's ladder.js twin: straight down from 1 to 3 km the canopy reads
// -0.07 to +0.03 stop against its plants, and 20 degrees down from 600 m
// and 1.2 km (tilt=70) +0.04 to +0.12, where its real plants and its
// painted crowns agree within 0.01; at 0.75 and 0.75 from a tenth to 0.4
// of the vertical it read 0.1 to 0.15 dark straight down and 0.3 to 0.47
// bright along the slant, a bright band where the land took over.)
const float CANOPY_SHADE = 0.87;
const float CANOPY_GRAZING = 0.46;
const float CANOPY_CELL = 6.0;
const float CANOPY_MOTTLE = 0.15;
const float CANOPY_DOME = 0.45;     // a crown's radius, of its cell
const float CANOPY_RELIEF = 0.9;    // its tone's swing about its mean
// (The dome's mean over its cell: its disc's share, pi r^2, by a dome's
// mean height over its disc, 2/3.)
const float CANOPY_DOME_MEAN = 3.14159265 * CANOPY_DOME * CANOPY_DOME * (2.0 / 3.0);
float canopySurface(vec3 worldPos, vec3 albedo, float shown, inout vec3 baseColor, inout vec3 N,
                    inout float roughness) {
    vec2 p = (worldPos.xz + camera.eyeWorld.xz) / CANOPY_CELL;
    vec2 c = floor(p);
    float h = fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);
    float h2 = fract(sin(dot(c, vec2(269.5, 183.3))) * 43758.5453);
    float h3 = fract(sin(dot(c, vec2(419.2, 371.9))) * 43758.5453);
    float footprint = max(length(dFdx(p)), length(dFdy(p)));
    float seen = 1.0 - smoothstep(0.25, 0.5, footprint);
    // A crown stands in the cell or does not, as a plant does, by the share
    // of the canopy shown there (its cover along the eye's line): the same
    // share of the cells as of the ground, the gaps between the stands'
    // crowns the ground's colour. (Its colour and its own shade only: the
    // ground's shade among the crowns, its AO and its sun, take the share,
    // as they did, or the gaps took all of the shade between crowns and the
    // woods read half a stop dark.)
    // Past where a cell spans half a pixel, the crowns a pixel holds: a
    // block of n cells holds a share of them that strays from the mean by
    // sqrt(s (1 - s) / n), drawn per block on the world (blocks of 2^k
    // cells, two sizes blended by the footprint), so the woods keep the
    // speckle of their crowns there rather than going smooth, and nothing
    // crawls as the eye moves.
    float s = shown;
    float lv = log2(max(2.0 * footprint, 1.0));
    float k0 = floor(lv);
    float spread = 0.0;
    for (int i = 0; i < 2; ++i) {
        float k = k0 + float(i);
        float size = exp2(k);
        vec2 b = floor(p / size);
        float u = fract(sin(dot(b, vec2(157.3, 113.1)) + 17.0 * k) * 43758.5453);
        spread += (i == 0 ? 1.0 - (lv - k0) : lv - k0) * 1.7320508 * (2.0 * u - 1.0) / size;
    }
    float far_ = clamp(s + sqrt(max(s * (1.0 - s), 0.0)) * spread, 0.0, 1.0);
    shown = mix(far_, h3 < shown ? 1.0 : 0.0, seen);
    // The cell's crown: its middle off the cell's by its hash, within it.
    vec2 off = p - c - (0.5 + (1.0 - 2.0 * CANOPY_DOME) * (vec2(h, h2) - 0.5));
    float rr = dot(off, off) / (CANOPY_DOME * CANOPY_DOME);
    float dome = sqrt(max(1.0 - rr, 0.0));
    float mottle = (1.0 + CANOPY_MOTTLE * (2.0 * h - 1.0)) * (1.0 + CANOPY_RELIEF * (dome - CANOPY_DOME_MEAN));
    mottle = mix(1.0, mottle, seen);
    vec3 lean = rr < 1.0 ? vec3(off.x, 0.0, off.y) / CANOPY_DOME : vec3(0.0);
    float down = -worldPos.y / max(length(worldPos), 1e-3);
    float shade = CANOPY_SHADE * mix(CANOPY_GRAZING, 1.0, smoothstep(0.1, 0.9, down));
    baseColor = mix(baseColor, albedo * mottle, shown);
    N = normalize(mix(N, normalize(N + vec3(0.0, 1.0, 0.0) + lean * seen), shown));
    roughness = mix(roughness, 1.0, shown);
    return mix(1.0, shade, shown);
}

void main() {
    uint flags = meshFlags();
    vec3 worldPos = inWorldPos;
    bool frontFacing = gl_FrontFacing;
#ifdef SCENE_VEGETATION
    gl_SampleMask[0] = -1;
#endif
    float subpixel = 0.0;
#ifdef SCENE_HERB
    // A herb's card as the world's module cuts and shades it
    // (scene_herb_layer.glsl's herbShade), its cover kept as that share of
    // the samples, each card its own dither. A card (a leaf, a bloom) a few
    // pixels long has parts a pixel or less (by its length: a stem is a thin
    // card, but a line, not a speck).
    float herbPx = 1.0 / max(min(fwidth(inUV.x), fwidth(inUV.y)), 1e-5);
    HerbShade herb = herbShade(HerbFragment(worldPos, inNormal, inTangent, inBitangent, inUV, inInstColor.rgb,
                                            inVegShade, inHerbInfo.xyz, inColor, inHerbSize, inHerbStand));
    if (!MESH_COVER_SHADE) {
        if (herb.cover < 1.0 / 255.0) discard;
        int mask = coverSampleMask(herb.cover, (flags & MESH_MSAA) != 0u, uint(inHerbInfo.w * 16777216.0));
        if (mask == 0) discard;
        gl_SampleMask[0] = mask;
    }
    // The pixels of such a card, and the part-covered ones (its edges, its
    // fine cuts) of one up to a dozen pixels across: the dither's
    // all-or-nothing pixels, which the sub-pixel resolve blends.
    subpixel = max(1.0 - smoothstep(2.0, 4.0, herbPx),
                   subpixelDecided(herb.cover) * (1.0 - smoothstep(8.0, 16.0, herbPx)));
#endif
#ifdef SCENE_GRASS
    // A blade as the world's sward module cuts and shades it
    // (scene_sward_layer.glsl's swardShade), with a far herb's bloom it
    // carries over it (grassSpeck); a part cover kept as that share of the
    // samples, each blade its own dither. Its pixels where a cut part is a
    // pixel or so across, and the part-covered ones of one up to a few
    // pixels: the dither's all-or-nothing pixels, which the sub-pixel
    // resolve blends.
    SwardShade sward = swardShade(SwardFragment(worldPos, frontFacing ? inNormal : -inNormal, inTangent, inUV, inColor,
                                                push.baseColor.rgb * inInstColor.rgb, inVegShade.xyz,
                                                floor((-inVegShade.w - 2.0) * 0.5) / 15.0, push.pbrParams.y));
    if (inInstColor.a > 0.5) {
        float speck = grassSpeck(inUV);
        sward.albedo = mix(sward.albedo, push.baseColor.rgb * grassUnpackColor(inInstColor.a), speck);
        sward.cover = max(sward.cover, speck);
    }
    if (!MESH_COVER_SHADE && sward.cover < 1.0) {
        int mask = coverSampleMask(sward.cover, (flags & MESH_MSAA) != 0u, uint(sward.seed * 131072.0));
        if (mask == 0) discard;
        gl_SampleMask[0] = mask;
    }
    subpixel = max(1.0 - smoothstep(1.5, 3.0, sward.px),
                   subpixelDecided(sward.cover) * (1.0 - smoothstep(3.0, 8.0, sward.px)));
#endif
    float camDist = length(worldPos);
    if (MESH_CUTS && push.extra.x > 0.0 && camDist < push.extra.x) discard;
    if (MESH_COVER_DEPTH) return;

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
#ifdef SCENE_GRASS
    baseColor = sward.albedo;
#endif
#ifdef SCENE_HERB
    baseColor = herb.albedo;
    alpha = 1.0;
#endif
#ifdef SCENE_VEGETATION
    // A grass blade (mesh_grass.vert) marks itself with w <= -2: w = -(2 +
    // ao + 2k), k (0..15) how far its translucency has faded with distance.
    // Translucent like a leaf, with no paler underside.
    bool vegLeaf = inVegShade.w < 0.0;
    bool vegBlade = inVegShade.w < -1.5;
    float vegBladeT = -inVegShade.w - 2.0;
    float vegFade = vegBlade ? floor(vegBladeT * 0.5) : 0.0;
    float vegAO = vegBlade ? vegBladeT - 2.0 * vegFade : abs(inVegShade.w);
    // A leaf's face is the one its normal points to. Its frame (the clump
    // and scatter placements, clumpLeafFrame, bromesh's leaf scatter) is a
    // mirror image (across, normal, tip are left-handed), so its triangles
    // wind the other way and gl_FrontFacing names the wrong face: the eye
    // would see a sunlit top lit as its own underside, and a leaf against
    // the sun lit on the side away from it.
    if (vegLeaf && !vegBlade) frontFacing = dot(inNormal, -inWorldPos) >= 0.0;
    // A leaf's underside is paler and yellower than its top.
    vec3 vegUnder = baseColor * vec3(1.25, 1.3, 1.05) + vec3(0.015, 0.02, 0.0);
    if (vegLeaf && !vegBlade && !frontFacing) baseColor = mix(baseColor, vegUnder, 0.6);
#endif

    vec3 emissive = push.emissive.rgb * push.emissive.a;
    vec3 geomN = normalize(inNormal);
    SceneAir air = sceneAir(worldPos, camDist);

    // Unlit is the authored base colour as is: no lights and no emissive
    // (GL's unlit branch), only the air in front of it.
    if ((flags & MESH_UNLIT) != 0u) {
        vec3 color = baseColor * air.transmittance + air.inscatter;
        if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) color *= cellShade(worldPos, geomN);
        // Unlit surfaces reflect nothing in the SSR mask phase.
        outColor = vec4(color, (flags & MESH_REFLECTANCE) != 0u ? 0.0 : mix(alpha, 0.0, air.fade));
        outMark = meshMark(subpixel);
#ifdef SCENE_INDIRECT_OUTPUT
        outIndirect = vec4(0.0);
#endif
        return;
    }

    // Two-sided thin surfaces shade the face the camera sees.
    vec3 N = geomN;
    if ((flags & MESH_TWO_SIDED) != 0u && !frontFacing) N = -N;
#ifdef SCENE_LAND
    // The land's albedo and normal a fragment at a time, from its tiles at
    // the level whose texel is the fragment's footprint: the vertex's are a
    // ring cell's, several pixels across. inUV is the map point
    // (mesh_land.vert), small about the eye where a body point is not.
    //
    // Under its sea (the vertices stand on the water there) the fragment is
    // water as far as its own ground lies lower, its edge a pixel's soft
    // step of the ground's height: so a coast is as sharp as the pixels
    // whatever the tiles' texels. The water is flat, the seabed through it
    // dimmed along the light's way down and back (refracted into the water,
    // n 1.33) over the colour deep water sends back.
    float landWet = 0.0;
    {
        vec2 dm = max(abs(dFdx(inUV)), abs(dFdy(inUV)));
        float lambda = max(log2(max(dm.x, dm.y) / (0.5 * landCell0())), 0.0);
        float lh;
        vec3 ln;
        vec4 ls;
        bool landIn = landSampleNormal(inUV, lambda, lh, ln, ls);
        float lhStep = fwidth(lh);   // (in uniform control flow)
        if (landIn) {
            baseColor = push.baseColor.rgb * ls.rgb;
            N = ln;
            landWet = landSeaWet(lh, lhStep);
            if (landWet > 0.0) {
                baseColor = mix(baseColor, landSeaWater(inUV, lh, worldPos, baseColor), landWet);
                N = normalize(mix(N, landUp(inUV), landWet));
            }
        }
    }
#endif
#ifdef SCENE_GROUND
    // Turf over a land stands on the sea where the land does (its vertices
    // lifted to the water by landTop): the same water a fragment at a time,
    // over the land's own seabed, so the sea is not a flat shelf of turf.
    //
    // And where a pixel is tens of metres of ground every pattern the turf
    // works out (its blades' tops, tussocks, mottling, flowers) is far under
    // it: there the ground is what the land says it is, its surface's albedo
    // and normal lit as ground, handed over by the pixel's footprint. (The
    // sward lit as its blades seen from far above is their shaded depth, a
    // quarter as bright as the ground the land's albedo means.) Nearer, the
    // turf's patterns refine the land's colour, with no mean of their own:
    // the turf is scaled by the land's albedo over its own far look
    // (scene_ground.glsl's groundFarMean), so the land is what it hands
    // over to. (Its own colours were the meadow's in a wood, whose land is
    // the forest's: from 2 to 8 km straight down the planet's woods
    // darkened 0.6 of a stop as the land took over the turf.)
    float landWet = 0.0, landFar = 0.0;
    vec3 landSurface = vec3(-1.0);
    float groundFp = max(length(fwidth(worldPos)), 1e-4);
    vec2 landM = landMapOf(worldPos + landEye3());
    LandChart landC = landChartOf(landM);   // (the land's and the turf's)
    vec3 landWater = vec3(0.0), landAlbedo = vec3(0.0), landN = N;
    {
        vec2 dm = max(abs(dFdx(landM)), abs(dFdy(landM)));
        float lambda = max(log2(max(dm.x, dm.y) / (0.5 * landCell0())), 0.0);
        float lh;
        vec4 ls;
        bool landIn = landSampleNormal(landM, landPosOf(landM, landC), lambda, lh, landN, ls);
        float lhStep = fwidth(lh);   // (in uniform control flow)
        // (The pixel's angle at the eye's distance: smooth, where a
        // triangle's own footprint would hand over a ring's facets.)
        float fp = camDist * length(fwidth(worldPos / max(camDist, 1e-6)));
        if (landIn) {
            landSurface = ls.rgb;
            landAlbedo = push.baseColor.rgb * ls.rgb;
            landFar = smoothstep(TURF_FAR_FROM, TURF_FAR_TO, fp);
            landWet = landSeaWet(lh, lhStep);
            if (landWet > 0.0) landWater = landSeaWater(landM, lh, worldPos, landAlbedo);
        }
    }
#endif
    if ((flags & (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) == (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) {
        vec3 nTS = texture(texNormal, inUV).xyz * 2.0 - 1.0;
        N = normalize(mat3(normalize(inTangent), normalize(inBitangent), N) * nTS);
    }
    vec3 shadePos = worldPos;
#ifdef SCENE_VEGETATION
    // The crown as one form: the leaf's own normal blended toward the bulk.
    {
        float bulkWeight = length(inVegShade.xyz);
        float bulkKeep = 1.0;
        vec3 vegShadeN = inVegShade.xyz;
        if (vegLeaf && !vegBlade) {
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
#ifdef SCENE_HERB
    N = herb.normal;
    vegAO *= herb.ao;
#endif
#ifdef SCENE_GRASS
    N = sward.normal;
#endif

    float metallic = push.pbrParams.x;
    float roughness = push.pbrParams.y;
    if ((flags & MESH_METALLIC_ROUGHNESS) != 0u) {
        vec4 mr = texture(texMetallicRoughness, inUV);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if ((flags & MESH_EMISSIVE_MAP) != 0u) emissive *= texture(texEmissive, inUV).rgb;
#ifdef SCENE_HERB
    roughness = herb.roughness;
#endif
#ifdef SCENE_GRASS
    roughness = sward.roughness;
#endif
#ifdef SCENE_BARK
    // A tube's wood (scene_bark.glsl): inColor where on the stem, inUV its
    // height over the root and the bark word (negative: not a plant's).
    if (inUV.y >= 0.0) barkSurface(inColor, inUV.x, inUV.y, worldPos, N, inTangent, baseColor, N, roughness, vegAO);
#endif
#ifdef SCENE_GROUND
    // Turf (scene_ground.glsl): the vertex colour is the ground's data
    // (density, dryness, floor) where the grass node's field does not reach.
    // Where the land has it all (landFar 1) none of it is worked out.
    float groundAO = 1.0;
    vec4 groundThin = vec4(0.0), groundSward = vec4(0.0);
    float landRoughness = roughness;
    if (landFar < 1.0) {
        vec3 farMean;
        groundSurface(worldPos, groundFp, inColor, landC, baseColor, N, roughness, groundAO, groundThin, groundSward,
                      farMean);
        if (landSurface.x >= 0.0) baseColor *= clamp(landSurface / max(farMean, vec3(1e-3)), 0.0, 8.0);
        baseColor *= push.baseColor.rgb;
    }
    if (landFar > 0.0) {
        baseColor = mix(baseColor, landAlbedo, landFar);
        N = normalize(mix(N, landN, landFar));
        roughness = mix(roughness, landRoughness, landFar);
        groundAO = mix(groundAO, 1.0, landFar);
        groundThin.w *= 1.0 - landFar;
        groundSward.w *= 1.0 - landFar;
    }
    if (landWet > 0.0) {
        baseColor = mix(baseColor, landWater, landWet);
        N = normalize(mix(N, landUp(landM), landWet));
        groundAO = mix(groundAO, 1.0, landWet);
        groundThin.w *= 1.0 - landWet;
        groundSward.w *= 1.0 - landWet;
    }
#endif
#if defined(SCENE_LAND) || defined(SCENE_GROUND)
    // Waves under a pixel are slopes it averages: a sea seen from afar
    // glints over the spread of its waves' slopes (Cox and Munk's, about
    // 0.2 for a moderate wind: a perceptual roughness near 0.5), a broad soft
    // glint, not the near ripples' hot spot a few pixels wide.
    float seaPixel = length(fwidth(worldPos));
    float seaRough = mix(landSeaRoughness(), max(landSeaRoughness(), 0.5), smoothstep(0.5, 50.0, seaPixel));
    roughness = mix(roughness, seaRough, landWet);
#endif
    // A land's plants past their range (mesh_land.vert): their canopy over
    // the ground as far as they cover it, in place of the ground's own.
    vec4 canopy = vec4(0.0);
#if defined(SCENE_LAND) || defined(SCENE_GROUND)
    if ((flags & MESH_CANOPY) != 0u) canopy = landCanopyAt(worldPos);
#endif
    float canopyShown = canopy.a;
    float canopyShade = 1.0;
    if (canopyShown > 0.0) {
        canopyShade = canopySurface(worldPos, canopy.rgb, canopyShown, baseColor, N, roughness);
#ifdef SCENE_GROUND
        groundAO = mix(groundAO, 1.0, canopyShown);
        groundThin.w *= 1.0 - canopyShown;
#endif
    }

#ifdef CUSTOM_FRAGMENT
    userFragment(baseColor, N, metallic, roughness, emissive, alpha);
    N = normalize(N);
#endif

    SceneSurface s;
    s.position = shadePos;
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
    if (vegLeaf) s.translucency = (push.extra.y > 0.0 ? min(1.4 * push.extra.y, 1.0) : 0.6) * (1.0 - vegFade / 15.0);
#endif
    s.thinNormal = vec3(0.0);
    s.sward = vec4(0.0);
    s.landShade = 1.0;
#if defined(SCENE_LAND) || defined(SCENE_GROUND)
    // The ground among the plants past their cast range, as much of it as
    // shows past the canopy.
    if ((flags & MESH_CANOPY) != 0u) s.landShade = mix(landCanopyGround(worldPos), 1.0, canopyShown);
#endif
#ifdef SCENE_HERB
    s.translucency = herb.translucency >= 0.0 ? herb.translucency : s.translucency * -herb.translucency;
    s.sward = herb.sward;
#endif
#ifdef SCENE_GRASS
    // A blade is thin: the light comes through it by its own face, not by
    // the bulk it is lit through further off (which faces a high sun and
    // would never let it glow).
    s.thinNormal = dot(geomN, -worldPos) >= 0.0 ? geomN : -geomN;
    s.translucency = sward.translucency >= 0.0 ? sward.translucency : min(s.translucency * -sward.translucency, 1.0);
    s.sward = sward.sward;
#endif
#ifdef SCENE_GROUND
    // Where the turf stands for far blades, it is lit as they are, the
    // light coming through them as through theirs.
    s.sward = groundSward;
    if (groundThin.w > 0.0) {
        s.translucency = groundThin.w;
        s.thinNormal = groundThin.xyz;
    }
#endif
    // A canopy seen from afar is lit as a sward is: a layer of leaves too
    // fine to resolve, the eye seeing the nearest along its line and of
    // those the ones the light reaches, standing up whatever the slope.
    if (canopyShown > 0.0)
        s.sward = vec4(normalize(mix(s.sward.w > 0.0 ? s.sward.xyz : N, vec3(0.0, 1.0, 0.0), canopyShown)),
                       mix(s.sward.w, 1.0, canopyShown));

    SceneProbe probe = sceneProbe(shadePos);
    vec3 indirect = sceneAmbient(s, probe);
    if ((flags & MESH_OCCLUSION_MAP) != 0u) indirect *= texture(texOcclusion, inUV).r;
#ifdef SCENE_VEGETATION
    indirect *= vegAO;
#endif
#ifdef SCENE_GROUND
    indirect *= groundAO;
#endif
#ifdef SCENE_HERB
    vec3 color = sceneDirectLight(s, probe) * herb.sun + indirect + emissive;
#else
    vec3 color = sceneDirectLight(s, probe) + indirect + emissive;
#endif

    // SSR mask phase: alpha carries the reflectance the SSR pass weights its
    // reflections by (normal-incidence Fresnel luminance times smoothness^2);
    // the SSR pass restores coverage before anything blends against it.
    if ((flags & MESH_REFLECTANCE) != 0u) {
        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
        alpha = dot(F0, vec3(0.2126, 0.7152, 0.0722)) * (1.0 - s.roughness) * (1.0 - s.roughness);
    }

    color *= canopyShade;
    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    alpha = mix(alpha, 0.0, air.fade);

    if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) {
        float shade = cellShade(worldPos, geomN);
        color *= shade;
        indirect *= shade;
    }

    outColor = vec4(min(color, vec3(SCENE_HDR_MAX)), alpha);
    outMark = meshMark(subpixel);
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(min(indirect, vec3(SCENE_HDR_MAX)), 0.0);
#endif
}

#if defined(SCENE_HERB) || defined(SCENE_GROUND) || defined(SCENE_GRASS)
// The world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl), after
// all they may use.
#include "ground/sward_module.glsl"
#include "ground/herb_module.glsl"
#endif
