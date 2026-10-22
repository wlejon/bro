#version 450
// A tile of the turf's surface layer (scene_ground_layers.glsl): at each
// texel, scene_ground.glsl's GroundBake of the ground's data there,
// band-limited to the texel (scene_mesh_drawer_ground.cpp).
//
// On a flat body the tile is filled by drawing the ground's meshes straight
// down into it (mesh.vert's static build under an orthographic camera
// fitted to the tile and its border). On a round body (GROUND_LAYER_CHART)
// the tile is a square of the land's chart, filled whole
// (ground_chart_fill.vert): a texel's point is its chart's, and its
// patterns are the lattice cell's holding it, blended across a cell's edge
// as the turf's fragments blend them (scene_grass.glsl's coverTurfCells).
// push.model's first column is the tile's corner on the chart (from the
// anchor's point on the face when its second column's y is 1, else from the
// face's middle), its texel and its border; the second's x its face.

#ifndef GROUND_LAYER_CHART
layout(location = 0) in vec3 inWorldPos;
layout(location = 3) in vec4 inColor;
#endif

layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;
layout(location = 2) out vec4 outC;
layout(location = 3) out vec4 outD;

#define GROUND_LAYER_FILL 1
#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#include "ground/scene_grass.glsl"
#include "ground/scene_ground.glsl"

#ifdef GROUND_LAYER_CHART
void main() {
    vec4 tile = push.model[0];
    vec2 off = tile.xy + (gl_FragCoord.xy - tile.w) * tile.z;
    vec3 p = landPoint(landChartMap(int(push.model[1].x + 0.5), off, push.model[1].y > 0.5), 0.0);
    CoverCell cells[4];
    float w[4];
    int n = coverTurfCells(p, cells, w);
    GroundBake sum;
    sum.a = sum.b = sum.c = sum.d = vec4(0.0);
    for (int i = 0; i < n; ++i) {
        vec4 rows[4];
        coverCellFrame(cells[i], rows);
        coverUse(rows);
        vec2 xz = coverFlatOf(rows, p).xz;
        GroundBake b = groundBake(xz, groundData(xz, vec4(0.0)), tile.z);
        sum.a += w[i] * b.a;
        sum.b += w[i] * b.b;
        sum.c += w[i] * b.c;
        sum.d += w[i] * b.d;
    }
    coverDone();
    outA = sum.a;
    outB = sum.b;
    outC = sum.c;
    outD = sum.d;
}
#else
void main() {
    vec2 xz = inWorldPos.xz + camera.eyeWorld.xz;
    // A texel's side (the camera looks straight down: x and z each step one
    // texel a pixel).
    vec2 w = fwidth(inWorldPos.xz);
    float fp = max(max(w.x, w.y), 1e-4);
    GroundData g = groundData(xz, inColor);
    GroundBake b = groundBake(xz, g, fp);
    outA = b.a;
    outB = b.b;
    outC = b.c;
    outD = b.d;
}
#endif

// The world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl), after
// all they may use.
#include "ground/sward_module.glsl"
#include "ground/herb_module.glsl"
