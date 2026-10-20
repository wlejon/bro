// The turf's surface layer (docs/scene-world-architecture.md §5; the tiles
// are scene/surface_layers.h's, filled by ground_layer_fill.frag): the
// position-only patterns of scene_ground.glsl's GroundBake, kept in tiles a
// fragment samples instead of working them out. Included by
// scene_ground.glsl; the bindings are the ground set's past the grass
// node's own three (scene_mesh_drawer_grass.cpp's groundSet).

// A slot an array layer, each a tile with its border: GroundBake's a, b, c, d.
layout(set = 4, binding = 4) uniform sampler2DArray groundLayerA;
layout(set = 4, binding = 5) uniform sampler2DArray groundLayerB;
layout(set = 4, binding = 6) uniform sampler2DArray groundLayerC;
layout(set = 4, binding = 7) uniform sampler2DArray groundLayerD;
// A level a layer: an entry per tile (x, z mod the entries a side), holding
// its slot + 1 and the tile's x and z above the entry's own bits
// (SurfaceLayers::encodePage), 0 for none.
layout(set = 4, binding = 8) uniform usampler2DArray groundLayerPage;

// The finest tile's texel, metres (0 without layers: everything is worked
// out where it is drawn).
float groundLayerFinest() { return grass.layer.w > 0.5 ? grass.layer2.w : 0.0; }

// The slot of the tile of `level` holding `xz`, and `uv` there; -1 when no
// tile there is resident.
int groundLayerSlot(vec2 xz, int level, out vec2 uv) {
    float side = grass.layer.x * exp2(float(level));
    vec2 t = xz / side;
    ivec2 ti = ivec2(floor(t));
    int n = int(grass.layer2.z);
    uint e = texelFetch(groundLayerPage, ivec3(ti & (n - 1), level), 0).r;
    uv = vec2(0.0);
    if (e == 0u) return -1;
    uvec2 hi = uvec2(ti >> int(grass.layer2.y)) & 1023u;
    if (((e >> 12u) & 1023u) != hi.x || (e >> 22u) != hi.y) return -1;
    float texels = grass.layer.y, border = grass.layer.z;
    uv = (border + (t - vec2(ti)) * texels) / (texels + 2.0 * border);
    return int(e & 4095u) - 1;
}

GroundBake groundLayerFetch(vec2 uv, int slot) {
    vec3 p = vec3(uv, float(slot));
    GroundBake b;
    b.a = textureLod(groundLayerA, p, 0.0);
    b.b = textureLod(groundLayerB, p, 0.0);
    b.c = textureLod(groundLayerC, p, 0.0);
    b.d = textureLod(groundLayerD, p, 0.0);
    return b;
}

// The patterns at `xz` for a footprint `fp` from the tiles: the level
// whose texel is the selection's most pixels at `fp` (SurfaceLayerConfig::
// texelPixels), blended toward the next coarser one across the level, or
// the finest coarser level resident when that one is not. False when no
// level is.
bool groundLayerSample(vec2 xz, float fp, out GroundBake b) {
    b.a = b.b = b.c = b.d = vec4(0.0);
    int levels = int(grass.layer.w + 0.5);
    if (levels <= 0) return false;
    float lambda = clamp(log2(max(grass.layer2.x * fp / grass.layer2.w, 1e-6)), 0.0, float(levels - 1));
    int l0 = int(lambda);
    float f = lambda - float(l0);
    vec2 uv;
    int slot = -1, level = l0;
    for (; level < levels; ++level) {
        slot = groundLayerSlot(xz, level, uv);
        if (slot >= 0) break;
    }
    if (slot < 0) return false;
    b = groundLayerFetch(uv, slot);
    if (level == l0 && f > 0.0 && l0 + 1 < levels) {
        vec2 uv1;
        int s1 = groundLayerSlot(xz, l0 + 1, uv1);
        if (s1 >= 0) {
            GroundBake c = groundLayerFetch(uv1, s1);
            b.a = mix(b.a, c.a, f);
            b.b = mix(b.b, c.b, f);
            b.c = mix(b.c, c.c, f);
            b.d = mix(b.d, c.d, f);
        }
    }
    return true;
}
