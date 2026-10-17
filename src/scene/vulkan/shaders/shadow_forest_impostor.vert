#version 450
// Forest impostor shadow casters: mesh_forest_impostor.vert's card, facing
// the light, reading the one baked view nearest the light's direction
// (shadow_forest_impostor.frag), so a far tree casts its crown's own
// outline and gaps at the depth of its leaves. Everything constant over the
// card is worked out here: a caster covers its whole shadow tile several
// deep, so its fragment is kept to a ray-plane hit and two reads.

layout(location = 0) out vec3 outOrigin;      // the card's point, specimen space, unswayed
layout(location = 1) out vec3 outRay;         // the light's ray there, specimen space (a world metre a unit of t)
layout(location = 2) out vec4 outDepth;       // the card point's light clip z and w, and their change along the ray
layout(location = 3) flat out vec4 outView;   // the view's direction (toward the light), the picture's half size
layout(location = 4) flat out vec4 outRight;  // its image's right axis, the bias (world)
layout(location = 5) flat out vec4 outCenter; // the picture's centre (specimen space)
layout(location = 6) flat out vec4 outAtlas;  // the view's atlas origin and a view's size (texels), the inset (uv)

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
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
    if (!forestImpostorTree(gl_VertexIndex / 6, casterModel(), t)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    mat4 L = push.lightViewProj;
    mat4 Li = inverse(L);
    float hc = (t.center.y - t.rootY) / max(t.height, 0.05);
    vec3 c = t.center + forestImpostorOffset(t.xt, t.xl, t.height, hc);
    // The light's ray through a clip-space point: from its depth 0 to 1.
    vec4 cc = L * vec4(c, 1.0);
    vec4 a = Li * vec4(cc.xy / cc.w, 0.0, 1.0), b = Li * vec4(cc.xy / cc.w, 1.0, 1.0);
    vec3 d = normalize(b.xyz / b.w - a.xyz / a.w);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), -d);
    right = dot(right, right) < 1e-8 ? vec3(1.0, 0.0, 0.0) : normalize(right);
    vec3 up = cross(-d, right);
    float reach = forestImpostorReach(t);
    float r = t.radius + reach;
    // At the sphere's light-facing side, so the written depth only moves
    // away from the light (shadow_forest_impostor.frag's depth_greater), as
    // large as the tree's upright cylinder seen from the light.
    vec2 k = forestImpostorExtent(t, -d, reach);
    vec3 pos = c - d * r + right * (corner.x * k.x) + up * (corner.y * k.y);
    vec4 clip = L * vec4(pos, 1.0);
    vec4 pa = Li * vec4(clip.xy / clip.w, 0.0, 1.0), pb = Li * vec4(clip.xy / clip.w, 1.0, 1.0);
    vec3 ray = normalize(pb.xyz / pb.w - pa.xyz / pa.w);

    // The one view nearest the light.
    ivec3 cells;
    vec3 w;
    forestImpostorViews(normalize(mat3(t.inv) * -d), t.atlas.views, cells, w);
    int cell = cells[w.y > w.x ? (w.z > w.y ? 2 : 1) : (w.z > w.x ? 2 : 0)];
    ivec2 ij = ivec2(cell % t.atlas.views, cell / t.atlas.views);
    vec3 f = forestImpostorDir(ij, t.atlas.views);
    vec3 vr, vu;
    forestImpostorBasis(f, vr, vu);
    float R = float(t.atlas.texels);

    gl_Position = clip;
    // Moved back by the sway at this corner's height, as mesh.frag does a
    // pixel's (here a corner's, across the card).
    float hp = (pos.y - t.rootY) / max(t.height, 0.05);
    outOrigin = (t.inv * vec4(pos - forestImpostorOffset(t.xt, t.xl, t.height, hp), 1.0)).xyz;
    outRay = mat3(t.inv) * ray;
    vec4 step = L * vec4(ray, 0.0);
    outDepth = vec4(clip.z, clip.w, step.z, step.w);
    // A little beyond the leaves (a twelfth of the picture's half size): a
    // surface the picture puts back lies within about that of the one its
    // shadow is cast from, and must not shade itself.
    outView = vec4(f, t.extent);
    outRight = vec4(vr, 0.08 * t.radius);
    outCenter = vec4(t.sphere, 0.0);
    outAtlas = vec4(vec2(t.atlas.block + ij) * R, R, 0.5 / R);
}
