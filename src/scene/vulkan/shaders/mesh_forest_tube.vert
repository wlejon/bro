#version 450
// Forest bark (scene_forest.glsl): each instance is a draw entry, a run of
// its tree's segments (thickest first, so a run from the first is always
// connected to the root) as tapered tube walls of `param` sides, built from
// the vertex index like mesh_tube.vert, mitred at the joints, and bending in
// the wind field. shadow_forest_tube.vert is the caster.
//
// The TWIG build (push.extra.z 2 + its rows a piece, the drawer's Twigs and
// TwigsFine): an entry is a run of one clump's twig pieces
// (scene_forest_shoot.glsl: its lobe stems and limbs, its sprays' twigs and
// axes, its shoots), `param` the clump, each piece a ribbon along the curve
// through its joints, facing the eye at the wood's own radius (capped at
// FOREST_TWIG_MAX_RADIUS), a pixel wide at the least: thinner than that its
// cover (mesh.frag's sample mask, through outUV.x) is its width in pixels,
// so a twig thins away with distance rather than popping; near, it is its
// own few millimetres, gone only where the eye all but touches it. Its
// points take the wood's pose and its spray's and shoot's bending exactly as
// the leaves on it do. Shaded as bark (mesh.frag's barkSurface, the
// specimen's bark word in outUV.y), darker into the crown.
//
// The BARK RIBBON build (a twig entry whose param is -1): an entry is a run
// of the tree's segments the round bark leaves out or thins (past its cut,
// the entry's `widening`, specimen units), each a ribbon along it facing the
// eye as a twig's is, its cover its width in pixels less the share the round
// bark still draws there. Left out, a far tree's fine branches went whole:
// from above an open crown's grey haze of them, a quarter of what it covers.

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
const vec2 FOREST_TWIG_NEAR = vec2(0.03, 0.08);
// ... and as the leaves on it shrink to their nodes (FOREST_NEAR_SHRINK,
// 0.1-0.4 m), it slims to FOREST_TWIG_SLIM of itself (whole from .y m, at
// the least by .x): a twig a few centimetres off is a slender stem across
// the view, not a rod as thick as a finger.
const vec2 FOREST_TWIG_SLIM_AT = vec2(0.05, 0.18);
const float FOREST_TWIG_SLIM = 0.45;

// A twig's collar: it starts (on the axis of the wood it grows from) at .x
// of its width, so its square end lies inside that wood rather than poking
// out beyond it, and swells to 1 + .y of it where it leaves that wood, .z
// of its radius on; then narrows on to itself.
const vec3 FOREST_TWIG_COLLAR = vec3(0.5, 0.0, 3.0);

void twigMain(ForestEntry e) {
    // Drawn indexed (the drawer's woodIndices): a piece is K + 1 rows of its
    // two sides along it, K the draw's (push.extra.z - 2): 1 for the plain
    // twigs, a row at each joint; more for a clump near the eye, whose
    // twigs' curves through their joints show (forestHermite), the first
    // row of a twig's first piece at its collar.
    int K = max(1, int(push.extra.z - float(FOREST_TWIG_ROWS_BIAS) + 0.5));
    int per = 2 * (K + 1);
    int piece = e.first + gl_VertexIndex / per;
    int local = gl_VertexIndex - (gl_VertexIndex / per) * per;
    int row = local >> 1;
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
    float sideSign = (local & 1) != 0 ? 1.0 : -1.0;
    // This row's point: the piece's start, its end, or between them on the
    // curve through both (only a row between them works out both ends).
    ForestTwigEnd o;
    int kind;
    ForestSpray s;
    ForestShoot sh;
    if (!forestTwigEnd(c, piece, row == K ? 1.0 : 0.0, o, kind, s, sh)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    // Left out whole (every vertex of a piece decides alike, or a triangle
    // would stretch to the culled corner), before any posing: by its start's
    // radius at the clump's distance.
    {
        bool orthoC = abs(camera.proj[3][3]) > 0.5;
        float dc = max(length((model * vec4(c.C, 1.0)).xyz), 1e-3);
        float pxc = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (orthoC ? 1.0 : dc);
        if (2.0 * min(o.start * vegScale(model), FOREST_TWIG_MAX_RADIUS) * pxc < FOREST_TWIG_PX.x) {
            gl_Position = FOREST_CULLED;
            return;
        }
    }
    float swell = 1.0;
    if (row > 0 && row < K) {
        ForestTwigEnd oa = o, ob;
        int kb;
        ForestSpray sb;
        ForestShoot shb;
        forestTwigEnd(c, piece, 1.0, ob, kb, sb, shb);
        {
            float f1 = 1.0 / float(K);
            if (oa.base) f1 = clamp(FOREST_TWIG_COLLAR.z * oa.radius / max(length(ob.p - oa.p), 1e-6), 0.02, f1);
            float f = row == 1 ? f1 : mix(f1, 1.0, float(row - 1) / float(K - 1));
            o.p = forestHermite(oa.p, oa.t, ob.p, ob.t, f, o.t);
            o.radius = mix(oa.radius, ob.radius, f);
            o.frac = mix(oa.frac, ob.frac, f);
            o.along = mix(oa.along, ob.along, f);
            if (oa.base && row == 1) swell = 1.0 + FOREST_TWIG_COLLAR.y;
        }
    } else if (row == 0 && o.base && K > 1) {
        swell = FOREST_TWIG_COLLAR.x;
    }

    // Posed as a leaf at this point would be (forestPoseLeaf): the wood,
    // then the spray's and shoot's bending.
    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(clump) + FOREST_CLUMP_VEGETATION / 4, o.p, 1.0, v0, v1, v2, v3);
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
    float slim = ortho ? 1.0 : mix(FOREST_TWIG_SLIM, 1.0, smoothstep(FOREST_TWIG_SLIM_AT.x, FOREST_TWIG_SLIM_AT.y, dist));
    float rw = min(o.radius * sc, FOREST_TWIG_MAX_RADIUS) * near * slim * swell;
    float widthPx = 2.0 * rw * pxPerM;
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
    // Round across: the normal sent is only its sideways part, so that
    // interpolated it is the point's offset across the ribbon (-1..1);
    // mesh.frag rounds it out toward the eye (a cylinder's normal there),
    // rather than two flat faces meeting along the middle.
    // (A hair toward the eye, so its middle still has a direction.)
    vec3 N = across * sideSign + toEye * 0.02;
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
    // along it, its radius), running on unbroken through the joints of a
    // stem, limb, twig or shoot (o.frac is along the whole of it); the
    // cover and the bark word in the UV (mesh.frag's twig, inUV.y under
    // -1.5).
    float ang = sideSign * 1.3 + PROC_TWO_PI * fract(float(clump) * 0.1031 + float(kind) * 0.37);
    float alongM = o.frac * 0.8 * c.Lf * sc;
    outColor = vec4(vec2(cos(ang), sin(ang)) * rw, alongM, rw);
    int segStart = int(floatBitsToUint(FOREST_FLOAT(forestSpecimenRecord(specimen), FOREST_SP_SEG_START)));
    float word = FOREST_FLOAT(forestSegmentRecord(segStart), FOREST_SEG_VEGETATION + FOREST_VEG_BARK);
    outUV = vec2(cover, -2.0 - max(word, 0.0));
    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T);
}

void barkRibbonMain(ForestEntry e) {
    // Two rows (its ends) of its two sides a segment, as a plain twig piece.
    int seg = e.first + gl_VertexIndex / 4;
    int local = gl_VertexIndex & 3;
    int r = forestSegmentRecord(seg);
    vec4 s0 = FOREST_RECORD(r, FOREST_SEG_FROM), s1 = FOREST_RECORD(r, FOREST_SEG_TO);
    vec3 axis = s1.xyz - s0.xyz;
    if (seg >= e.end || dot(axis, axis) < 1e-12 || forestSegmentBare(e.instance, seg)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    float endT = float(local >> 1);
    float sideSign = (local & 1) != 0 ? 1.0 : -1.0;
    vec3 centre = endT > 0.5 ? s1.xyz : s0.xyz;
    // (A tip's end radius is negative: it closes to a point.)
    float rad = endT > 0.5 ? max(s1.w, 0.0) : s0.w;
    vec4 v0, v1, v2, v3;
    forestVegetation(r + FOREST_SEG_VEGETATION / 4, centre, endT, v0, v1, v2, v3);
    vec3 pos = (model * vec4(centre, 1.0)).xyz;
    mat3 rot;
    outVegShade = vegetationSwayAxis(model, v0, v1, v2, v3, vec3(0.0), vec3(0.0, 1.0, 0.0), pos, rot);
    vec3 T = normalize(rot * (mat3(model) * axis));

    float dist = max(length(pos), 1e-3);
    bool ortho = abs(camera.proj[3][3]) > 0.5;
    float pxPerM = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (ortho ? 1.0 : dist);
    float sc = vegScale(model);
    float rw = rad * sc;
    float widthPx = 2.0 * rw * pxPerM;
    // The share of its width the round bark leaves (scene_forest_tube.glsl
    // thins it from 1.6 times the cut to nothing at it).
    // The drawer stops at its share of the cut (kBarkRibbonCut, the
    // entry's `tail`): they fade in to 1.5 times that, so none blinks out
    // as the eye backs off.
    float low = e.tail * e.widening;
    float left = e.widening > 0.0 ? (1.0 - smoothstep(e.widening, 1.6 * e.widening, rad)) *
                                        smoothstep(low, 1.5 * low, rad)
                                  : 0.0;
    float cover = clamp(widthPx * left, 0.0, 1.0);
    vec3 toEye = ortho ? normalize(vec3(camera.view[0][2], camera.view[1][2], camera.view[2][2])) : -pos / dist;
    vec3 across = cross(T, toEye);
    float al = length(across);
    across = al > 1e-5 ? across / al : perpendicularUnit(T);
    pos += across * (sideSign * max(rw, 0.5 / pxPerM));

    vec3 N = across * sideSign + toEye * 0.02;
    outInstColor = vec4(vegetationTint(model, v0, v1, v2, v3, false) * forestSpecimenTint(specimen, true), e.fade);
    vec4 barkRec = FOREST_RECORD(r, FOREST_SEG_VEGETATION + FOREST_VEG_WEIGHT_FROM);   // (weightFrom, weightTo, path, bark word)
    float ang = sideSign * 1.3;
    outColor = vec4(vec2(cos(ang), sin(ang)) * rw, (barkRec.z + endT * length(axis)) * sc, rw);
    outUV = vec2(cover, -2.0 - max(barkRec.w, 0.0));
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
        if (e.param < 0)
            barkRibbonMain(e);
        else
            twigMain(e);
        return;
    }
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    vec3 pos, radial, axisDir;
    vec2 uv;
    int seg;
    float endT;
    // (The rings' sides morph by their width: push.extra.z is minus the
    // pixels they do it by.)
    float rad;
    if (!forestTubeVertexMorph(e, gl_VertexIndex, model, -push.extra.z, pos, radial, axisDir, uv, seg, endT, rad)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    vec4 v0, v1, v2, v3;
    forestVegetation(forestSegmentRecord(seg) + FOREST_SEG_VEGETATION / 4, pos, endT, v0, v1, v2, v3);

    // The bark (scene_bark.glsl): where on the stem the point lies, the
    // trunk's foot split into buttresses.
    int r0 = forestSegmentRecord(seg);
    vec4 barkRec = FOREST_RECORD(r0, FOREST_SEG_VEGETATION + FOREST_VEG_WEIGHT_FROM);   // (weightFrom, weightTo, path, bark word)
    float ang = uv.x * PROC_TWO_PI;
    float hb = pos.y - v0.y;
    pos += radial * (rad * barkButtress(ang, hb, rad, treeSeed));
    float sc = vegScale(model);
    float along = barkRec.z + endT * length(FOREST_RECORD(r0, FOREST_SEG_TO).xyz - FOREST_RECORD(r0, FOREST_SEG_FROM).xyz);
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
