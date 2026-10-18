#version 450
// Forest bark (scene_forest.glsl): each instance is a draw entry, a run of
// its tree's segments (thickest first, so a run from the first is always
// connected to the root) as tapered tube walls of `param` sides, built from
// the vertex index like mesh_tube.vert, mitred at the joints, and bending in
// the wind field. shadow_forest_tube.vert is the caster.
//
// The TWIG build (push.extra.z 3, the drawer's Twigs): an entry is a run of
// one clump's twig pieces (scene_forest_shoot.glsl: its lobe stems and
// limbs, its sprays' twigs and axes, its shoots), `param` the clump, each
// piece a ribbon facing the eye at the wood's own radius (capped at
// FOREST_TWIG_MAX_RADIUS), a pixel wide at the least: thinner than that its
// cover (mesh.frag's sample mask, through outUV.x) is its width in pixels,
// so a twig thins away with distance rather than popping; near, it is its
// own few millimetres, gone only where the eye all but touches it. Its
// points take the wood's pose and its spray's and shoot's bending exactly as
// the leaves on it do. Shaded as bark (mesh.frag's barkSurface, the
// specimen's bark word in outUV.y), darker into the crown.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
layout(location = 7) out vec4 outVegShade;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#include "scene_forest_tube.glsl"
#include "scene_forest_shoot.glsl"
#define SCENE_BARK_VERTEX 1
#include "scene_bark.glsl"

// A twig under this many pixels across is not drawn; to a pixel it fades
// in (its cover its width).
const vec2 FOREST_TWIG_PX = vec2(0.15, 0.4);
// A twig's radius at most (m): thicker wood is the tree's own branches.
const float FOREST_TWIG_MAX_RADIUS = 0.007;
// A twig thins away only where the eye all but touches it (gone by .x,
// whole from .y m), inside the leaves' own FOREST_NEAR_SHRINK: up close it
// is wood at its own few millimetres, many pixels across, holding up the
// leaves on it.
const vec2 FOREST_TWIG_NEAR = vec2(0.03, 0.1);

void twigMain(ForestEntry e) {
    int piece = e.first + gl_VertexIndex / 6;
    int corner = gl_VertexIndex - (gl_VertexIndex / 6) * 6;
    int clump = e.param;
    if (piece >= e.end || procClumpBare(e.instance, clump)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    forestApplyClumpFlat(e.instance);
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    ForestClumpShoots c = forestClumpShoots(clump, int(forestSpecimenShoot(specimen).y + 0.5));
    // corner 0=a- 1=a+ 2=b-   3=b- 4=a+ 5=b+ (a the piece's start, b its end)
    bool top = corner == 2 || corner == 3 || corner == 5;
    float sideSign = corner == 1 || corner == 4 || corner == 5 ? 1.0 : -1.0;
    ForestTwigEnd o;
    int kind;
    ForestSpray s;
    ForestShoot sh;
    if (!forestTwigEnd(c, piece, top ? 1.0 : 0.0, o, kind, s, sh)) {
        gl_Position = FOREST_CULLED;
        return;
    }

    // Posed as a leaf at this point would be (forestPoseLeaf): the wood,
    // then the spray's and shoot's bending.
    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(clump) + 2, o.p, 1.0, v0, v1, v2, v3);
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 anchor = (model * vec4(o.p, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, anchor, clamp(v2.w, 0.0, 1.0));
    vec3 pos = anchor + pose.offset;
    if (kind > 0) {
        ForestShootSway sw = forestShootSwayAt(s, sh, pose.drag, forestFlutter(e.widening));
        vec3 om;
        vec3 off = kind == 1 ? forestSprayBend(sw, s, o.along) : forestShootBend(sw, s, sh, o.along, om);
        pos += mat3(model) * off;
    }

    // A ribbon across the eye's line, a pixel wide at the least. Its radius
    // is wood's, whatever the leaf: never past FOREST_TWIG_MAX_RADIUS. It
    // thins away only within FOREST_TWIG_NEAR of the eye.
    float dist = max(length(pos), 1e-3);
    bool ortho = abs(camera.proj[3][3]) > 0.5;
    float pxPerM = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (ortho ? 1.0 : dist);
    float sc = vegScale(model);
    float near = ortho ? 1.0 : smoothstep(FOREST_TWIG_NEAR.x, FOREST_TWIG_NEAR.y, dist);
    float rw = min(o.radius * sc, FOREST_TWIG_MAX_RADIUS) * near;
    float widthPx = 2.0 * rw * pxPerM;
    // Left out whole (every vertex of a piece decides alike, or a triangle
    // would stretch to the culled corner): by its start's radius at the
    // clump's distance.
    {
        ForestTwigEnd o0;
        float r0 = o.radius;
        if (top) {
            int k0;
            ForestSpray s0;
            ForestShoot sh0;
            forestTwigEnd(c, piece, 0.0, o0, k0, s0, sh0);
            r0 = o0.radius;
        }
        float dc = max(length((model * vec4(c.C, 1.0)).xyz), 1e-3);
        float pxc = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (ortho ? 1.0 : dc);
        if (2.0 * min(r0 * sc, FOREST_TWIG_MAX_RADIUS) * pxc < FOREST_TWIG_PX.x) {
            gl_Position = FOREST_CULLED;
            return;
        }
    }
    // Across the curve's tangent at the joint, which both pieces meeting
    // there share, so the ribbon is unbroken through a crooked joint.
    vec3 T = normalize(vegRotation(pose.omega) * (mat3(model) * o.t));
    vec3 toEye = ortho ? normalize(vec3(camera.view[0][2], camera.view[1][2], camera.view[2][2])) : -pos / dist;
    vec3 across = cross(T, toEye);
    float al = length(across);
    across = al > 1e-5 ? across / al : perpendicularUnit(T);
    float halfWidth = max(rw, 0.5 / pxPerM);
    pos += across * (sideSign * halfWidth);

    // (Close to the eye a thinned twig is still a pixel or so across: it
    // fades with its thinning too, not a wire.)
    float cover = clamp(widthPx, 0.0, 1.0) * smoothstep(FOREST_TWIG_PX.x, FOREST_TWIG_PX.y, widthPx) *
                  clamp(e.keepEnd, 0.0, 1.0) * near * near;
    // Round across: its normal leans out to the side it is on.
    vec3 N = normalize(across * (0.8 * sideSign) + toEye * 0.6);
    // Bark (mesh.frag's barkSurface, as the tubes'): this year's shoots
    // still green-brown toward their tips, the older wood the tree's bark
    // (a birch's pale bark red-brown on wood this thin). Lit as the tubes
    // are, darkening toward the ground and into the crown.
    vec3 green = kind == 2 ? mix(vec3(1.0), vec3(0.8, 0.95, 0.6), 0.3 + 0.7 * o.frac) : vec3(1.0);
    vec4 shade = vegCrownShade(model, v1, anchor);
    float h = clamp((anchor.y - plant.root.y) / plant.height, 0.0, 1.0);
    // Halfway from the leaves' occlusion there to the branches' none: the
    // leaves' alone (they glow with the light through them, wood does not)
    // left the wood inside a crown near black beside the branch it grew on.
    float crownAO = shade.w < 0.0 ? -shade.w : 1.0;
    outVegShade = vec4(0.0, 0.0, 0.0, mix(0.8, 1.0, h) * mix(crownAO, 1.0, 0.5));
    outInstColor = vec4(vegetationTint(model, v0, v1, v2, v3, false) * forestSpecimenTint(specimen, true) * green,
                        e.fade);
    // Where on the wood (scene_bark.glsl's coordinates: round the stem,
    // along it, its radius), each piece its own stretch of bark; the cover
    // and the bark word in the UV (mesh.frag's twig, inUV.y under -1.5).
    float ang = sideSign * 1.3 + PROC_TWO_PI * fract(float(piece) * 0.618034 + float(clump) * 0.1031);
    float alongM = (float(piece) * 0.37 + o.frac * 0.2) * c.Lf * sc;
    outColor = vec4(vec2(cos(ang), sin(ang)) * rw, alongM, rw);
    int segStart = int(floatBitsToUint(procedural.records[forestSpecimenRecord(specimen)].x));
    float word = procedural.records[forestSegmentRecord(segStart) + 5].w;
    outUV = vec2(cover, -2.0 - max(word, 0.0));
    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T);
}

void main() {
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    forestApplyLook();
    ForestEntry e = forestEntry(gl_InstanceIndex);
    if (push.extra.z > 2.5) {
        twigMain(e);
        return;
    }
    vec3 pos, radial, axisDir;
    vec2 uv;
    int seg;
    float endT;
    if (!forestTubeVertex(e, gl_VertexIndex, pos, radial, axisDir, uv, seg, endT)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    vec4 v0, v1, v2, v3;
    forestVegetation(forestSegmentRecord(seg) + 3, pos, endT, v0, v1, v2, v3);

    // The bark (scene_bark.glsl): where on the stem the point lies, the
    // trunk's foot split into buttresses.
    int r0 = forestSegmentRecord(seg);
    vec4 barkRec = procedural.records[r0 + 5];   // (weightFrom, weightTo, path, bark word)
    float rad = mix(procedural.records[r0].w, procedural.records[r0 + 1].w, endT);
    float ang = uv.x * PROC_TWO_PI;
    float hb = pos.y - v0.y;
    pos += radial * (rad * barkButtress(ang, hb, rad, treeSeed));
    float sc = vegScale(model);
    float along = barkRec.z + endT * length(procedural.records[r0 + 1].xyz - procedural.records[r0].xyz);
    outColor = vec4(vec2(cos(ang), sin(ang)) * (rad * sc), along * sc, rad * sc);
    outUV = vec2(hb * sc, barkRec.w);

    vec4 world = model * vec4(pos, 1.0);
    mat3 M3 = mat3(model);
    vec3 N = normalize(M3 * radial);
    vec3 T = normalize(M3 * axisDir);
    mat3 rot;
    outVegShade = vegetationSwayAxis(model, v0, v1, v2, v3, vec3(0.0), N, world.xyz, rot);
    outInstColor = vec4(vegetationTint(model, v0, v1, v2, v3, false) * forestSpecimenTint(specimen, true), e.fade);
    N = rot * N;
    T = rot * T;

    outWorldPos = world.xyz;
    gl_Position = camera.viewProj * world;
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T);
}
