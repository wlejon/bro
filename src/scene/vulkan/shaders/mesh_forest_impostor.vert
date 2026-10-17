#version 450
// Forest impostors (scene_forest_impostor.glsl): one card a tree, entry
// gl_VertexIndex / 6 (the draw is not instanced), facing the eye and wide
// enough to hold the specimen's picture wherever the sway takes it. The
// fragment shader (mesh.frag built with SCENE_FOREST_IMPOSTOR) traces each
// pixel's ray into the picture; this hands it the tree: its inverse
// transform, the three views it is seen from, its pose and colour.
// shadow_forest_impostor.vert is the caster.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;      // the crown's centre, camera-relative
layout(location = 2) out vec2 outUV;          // the crown's radius, the root's height (camera-relative)
layout(location = 3) out vec4 outColor;       // x the drag at the tree now
layout(location = 4) out vec3 outTangent;     // the bark's per-tree tint
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;   // the leaves' per-tree tint, the fade code
layout(location = 7) out vec4 outVegShade;    // xyz the crown's limb deflection
layout(location = 8) flat out vec4 outInv0;   // specimen from camera-relative world: three rows
layout(location = 9) flat out vec4 outInv1;
layout(location = 10) flat out vec4 outInv2;
layout(location = 11) flat out ivec4 outCells;  // three views, the block's first cell (x + y * 65536)
layout(location = 12) flat out vec4 outWeights; // the views' weights, the picture's half size
layout(location = 13) flat out vec4 outSphere;  // its centre (specimen space), views + texels * 256
layout(location = 14) flat out vec4 outTrunk;   // the trunk tip's deflection, the tree's height

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#define FOREST_IMPOSTOR_VERTEX
#include "scene_forest_impostor.glsl"
#include "scene_forest_impostor_vertex.glsl"

void main() {
    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                     vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 corner = kCorners[gl_VertexIndex % 6];
    ForestImpostorTree t;
    outVegShade = vec4(0.0);
    if (!forestImpostorTree(gl_VertexIndex / 6, push.model, t)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    // The card faces the eye from where the picture stands now, wide enough
    // for the cone the picture's sphere fills from here.
    float hc = (t.center.y - t.rootY) / max(t.height, 0.05);
    vec3 c = t.center + forestImpostorOffset(t.xt, t.xl, t.height, hc);
    float reach = forestImpostorReach(t);
    float r = t.radius + reach;
    float D = length(c);
    vec3 f = D > 1e-4 ? -c / D : vec3(0.0, 0.0, 1.0);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), f);
    right = dot(right, right) < 1e-8 ? camera.invView[0].xyz : normalize(right);
    vec3 up = cross(f, right);
    // The card stands at the front of the sphere, so every surface the
    // picture puts back lies behind it: mesh.frag's written depth only ever
    // moves away from the eye, and the depth test can still run early. Its
    // size holds both the cone of the sphere and the tree's upright
    // cylinder seen from here, whichever is the tighter on each axis.
    float front = max(D - r, 0.5 * D);
    float kSphere = r * front * inversesqrt(max(D * D - r * r, 0.04 * r * r));
    vec2 k = min(forestImpostorExtent(t, f, reach) * (front / max(D - r, 0.05 * D)), vec2(kSphere));
    vec3 pos = c * (front / max(D, 1e-4)) + right * (corner.x * k.x) + up * (corner.y * k.y);

    // The views it is seen from: the eye's direction in specimen space.
    ivec3 cells;
    vec3 w;
    forestImpostorViews(normalize(mat3(t.inv) * -t.center), t.atlas.views, cells, w);

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = t.crown;
    outUV = vec2(t.crownRadius, t.rootY);
    outColor = vec4(t.gust, 0.0, 0.0, 1.0);
    outTangent = t.barkTint;
    outBitangent = vec3(0.0);
    outInstColor = vec4(t.leafTint, t.fade);
    outVegShade = vec4(t.xl, 0.0);
    mat4 it = transpose(t.inv);
    outInv0 = it[0];
    outInv1 = it[1];
    outInv2 = it[2];
    outCells = ivec4(cells, t.atlas.block.x + t.atlas.block.y * 65536);
    outWeights = vec4(w, t.extent);
    outSphere = vec4(t.sphere, float(t.atlas.views + t.atlas.texels * 256));
    outTrunk = vec4(t.xt, t.height);
}
