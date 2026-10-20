#version 450
// Plants' shadow casters (scene_plants.glsl): their own chunks (fewer,
// bigger leaves than the camera's) into an atlas tile, a leaf's corners read
// from its pose as plants.vert's are. push.params.x: 0 the leaves, 1 the
// wood (the colour draw's push.extra.z).

#include "common/scene_camera.glsl"
#include "common/scene_shadow_push.glsl"
#define PLANT_POSES_READ
#include "foliage/scene_plants.glsl"

void main() {
    bool wood = push.params.x > 0.5;
    int perElement = wood ? PLANT_TUBE_VERTS : PLANT_LEAF_POINTS;
    int c = gl_VertexIndex % perElement;
    vec3 pos;
    bool ok;
    vec4 chunk = plantChunks.chunks[gl_InstanceIndex];
    if (wood) {
        int k = int(chunk.y) + gl_VertexIndex / perElement;
        PlantVertex o;
        ok = float(k) < chunk.z && plantWood(int(chunk.x), k, c, o);
        pos = o.pos;
    } else {
        int e = gl_VertexIndex / perElement;
        if (chunk.w > 1.5) {
            // A chunk of crown discs (chunk.x its first disc record, chunk.y
            // how many): a far tree's shadow is its crown's outline, a
            // hexagon square to the light about its centre.
            ok = float(e) < chunk.y;
            if (ok) {
                int pi = int(plantChunks.chunks[int(chunk.x) + e].x);
                Plant p = plantAt(pi);
                PlantSpecies s = plantSpecies(p.species);
                PlantCrown cr = plantCrown(s, p.scale);
                vec3 centre = plantToWorld(p, cr.center) + plantSway(p, cr.center.y, s.height * p.scale, 0.0) -
                              camera.eyeWorld.xyz;
                // The light's own axes: its view-projection's first two rows.
                vec3 right = normalize(vec3(push.lightViewProj[0][0], push.lightViewProj[1][0], push.lightViewProj[2][0]));
                vec3 up = normalize(vec3(push.lightViewProj[0][1], push.lightViewProj[1][1], push.lightViewProj[2][1]));
                // A hexagon on the leaf polygon's 6 points and its indices
                // (base, left 1, right 1, left 2, right 2, tip: round the ring
                // 0 1 5 2 4 3, so its 4 triangles tile it).
                const int ring[6] = int[6](0, 1, 5, 2, 4, 3);
                float a = float(ring[c]) * 1.0471976 + plantRand(p.seed, 31u) * 6.2831853;
                float r = cr.radii.x * 0.85;
                pos = centre + right * cos(a) * r + up * sin(a) * mix(r, cr.radii.y * 0.85, 0.5);
            }
        } else {
            vec2 q;
            ok = plantPosedCorner(gl_InstanceIndex * PLANT_LEAVES_PER_CHUNK + e, c, pos, q);
        }
    }
    if (!ok) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    gl_Position = push.lightViewProj * vec4(pos, 1.0);
}
