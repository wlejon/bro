#version 450
// Plants (scene_plants.glsl): an instance a chunk of one plant's elements
// (its leaves, or its trunk and limbs). An element's points are read from
// its pose (plants_pose.comp built it this frame); a tube's vertices are
// built here from its plant and its module's limb. No vertex streams.
// push.extra.x: 0 the leaves, 1 the wood, 2 the painted crowns.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outLeafNormal;
layout(location = 2) out vec3 outCrownNormal;
layout(location = 3) out vec3 outAlbedo;
layout(location = 4) flat out int outSpecies;
layout(location = 5) out vec4 outCrown;
layout(location = 6) out vec2 outShade;
layout(location = 7) out vec3 outInfo;   // x far, y 1 wood (2 a painted crown), z the outline's detail
layout(location = 8) out vec2 outLeafUV;

#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#define PLANT_POSES_READ
#include "foliage/scene_plants.glsl"

// A plant painted (past the scene's real band, the plan's painted records):
// its crown as one lobed disc facing the eye, `sides` triangles about its
// middle (the draw's push.extra.y: the plan's near list's 24, its far
// list's 6), its rim the envelope's outline seen from here, lumped by the
// plant's own seed. Lit as a dome: its middle toward the eye, its rim
// outward; coloured its species' mean, shifted by its tint, a daub
// standing for the whole crown.
// (Its rim a little within the envelope's outline: a crown's foliage leaves
// the ground showing at its edge, and a disc to the whole outline covered
// more than the real crown and read the woods' pattern worse.)
#define PLANT_PAINT_FILL 0.9

// The envelope's support in unit direction u (plant frame, y up, about its
// centre): how far it reaches along u. Across the view it is the outline's
// reach; toward the eye, its near side. (The ellipsoid's, whatever the
// shape: a cone's hull, its foot's disc and its apex, reached well past
// what a conifer's foliage fills.)
float plantCrownSupport(PlantCrown cr, vec3 u) {
    float rx = cr.radii.x, ry = cr.radii.y;
    return sqrt(rx * rx * dot(u.xz, u.xz) + ry * ry * u.y * u.y);
}

bool plantPainted(int pi, int v, int sides, out PlantVertex o) {
    o.wood = false;
    o.leafUV = vec2(0.0);
    o.outline = 0.0;
    o.far = 1.0;
    Plant p = plantAt(pi);
    PlantSpecies s = plantSpecies(p.species);
    o.species = p.species;
    PlantCrown cr = plantCrown(s, p.scale);
    vec3 centre = plantToWorld(p, cr.center) + plantSway(p, s, cr.center.y, 0.0) - camera.eyeWorld.xyz;
    vec3 toEye = -normalize(centre);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), toEye);
    right = dot(right, right) > 1e-6 ? normalize(right) : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(toEye, right);
    int tri = v / 3, corner = v % 3;
    // (Flat, halfway to the envelope's near side, so the disc is lit as the
    // crown's skin more than its core, and never stands much before the
    // crown, before the real plants nearer; flat, so two crowns overlapping
    // stack one before the other, where fans coned toward the eye cut
    // through each other in wedges. The yaw turns neither support: the
    // envelope is round about its axis.)
    vec3 n = toEye;
    vec3 front = centre + toEye * (0.5 * plantCrownSupport(cr, toEye));
    vec3 pos = front;
    if (corner > 0) {
        float t = 6.2831853 * float(tri + corner - 1) / float(sides);
        vec3 w = cos(t) * right + sin(t) * up;
        float a = 6.2831853 * plantRand(p.seed, 31u), b = 6.2831853 * plantRand(p.seed, 32u);
        // (Its outline lobed, then ragged: a crown's rim is clumps, not an
        // arc. The near list's 24 sides keep a crown hundreds of pixels
        // across from reading straight-edged.)
        float lumps = 1.0 + 0.12 * sin(3.0 * t + a) + 0.08 * sin(5.0 * t + b) + 0.06 * sin(9.0 * t + a + b) +
                      0.05 * (plantRand(p.seed, uint((tri + corner - 1) % sides) + 40u) - 0.5);
        // (A polygon of few sides as large as the circle it stands for, so
        // the two lists cover alike where they meet.)
        float area = sqrt(6.2831853 / (float(sides) * sin(6.2831853 / float(sides))));
        float reach = PLANT_PAINT_FILL * plantCrownSupport(cr, w) * lumps * area;
        pos = front + w * reach;
        // (Across the disc in the crown's horizontal radii, so the same on
        // every triangle: a point's uv is where it is, not which spoke.)
        o.leafUV = vec2(cos(t), sin(t)) * (reach / max(cr.radii.x, 1e-3));
        n = normalize(PLANT_PAINT_RIM * w + toEye);
    }
    o.pos = pos;
    o.leafNormal = n;
    o.crownNormal = n;
    o.albedo = s.albedo * max(1.0 + 0.35 * p.tint, 0.0);
    o.crown = vec4(centre, cr.radii.x);
    o.shade = vec2(cr.radii.y, 0.35);
    return true;
}

void main() {
    int mode = int(push.extra.x + 0.5);
    bool wood = mode == 1;
    int perElement = wood ? PLANT_TUBE_VERTS : PLANT_LEAF_POINTS;
    int c = gl_VertexIndex % perElement;
    PlantVertex o;
    bool ok;
    if (mode == 2) {
        ok = plantPainted(plantChunkIndex(plantChunks.chunks[gl_InstanceIndex].x), gl_VertexIndex,
                          int(push.extra.y + 0.5), o);
    } else if (wood) {
        vec4 chunk = plantChunks.chunks[gl_InstanceIndex];
        int k = int(chunk.y) + gl_VertexIndex / perElement;
        // (A wood chunk's z: the plant's tubes drawn, the trunk alone far off.)
        ok = float(k) < chunk.z && plantWood(plantChunkIndex(chunk.x), k, c, o);
    } else {
        ok = plantPosedLeaf(gl_InstanceIndex * PLANT_LEAVES_PER_CHUNK + gl_VertexIndex / perElement, c, o);
    }
    if (!ok) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    outWorldPos = o.pos;
    gl_Position = camera.viewProj * vec4(o.pos, 1.0);
    outLeafNormal = o.leafNormal;
    outCrownNormal = o.crownNormal;
    outAlbedo = o.albedo;
    outSpecies = o.species;
    outCrown = o.crown;
    outShade = o.shade;
    outInfo = vec3(o.far, mode == 2 ? 2.0 : wood ? 1.0 : 0.0, o.outline);
    outLeafUV = o.leafUV;
}

#include "foliage/plant_module.glsl"
