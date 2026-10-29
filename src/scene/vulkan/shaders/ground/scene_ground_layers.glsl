// The turf's surface layer (the tiles
// are scene/world/surface_layers.h's, filled by ground_layer_fill.frag): the
// position-only patterns of scene_ground.glsl's GroundBake, kept in tiles a
// fragment samples instead of working them out. Included by
// scene_ground.glsl; the bindings are the ground set's past the grass
// node's own three (scene_mesh_drawer_grass.cpp's groundSet).

// A slot an array layer, each a tile with its border: GroundBake's a, b, c, d.
// (Bare images, through scene_grass.glsl's customSamplers.)
layout(set = 4, binding = 4) uniform texture2DArray groundLayerAImage;
layout(set = 4, binding = 5) uniform texture2DArray groundLayerBImage;
layout(set = 4, binding = 6) uniform texture2DArray groundLayerCImage;
layout(set = 4, binding = 7) uniform texture2DArray groundLayerDImage;
#define groundLayerA sampler2DArray(groundLayerAImage, customSamplers[CS_LINEAR])
#define groundLayerB sampler2DArray(groundLayerBImage, customSamplers[CS_LINEAR])
#define groundLayerC sampler2DArray(groundLayerCImage, customSamplers[CS_LINEAR])
#define groundLayerD sampler2DArray(groundLayerDImage, customSamplers[CS_LINEAR])
// A level a layer, a root's levels together (root * grass.turf.w + level):
// an entry per tile (x, z mod the entries a side), holding
// its slot + 1 and the tile's x and z above the entry's own bits
// (SurfaceLayers::encodePage), 0 for none.
layout(set = 4, binding = 8) uniform utexture2DArray groundLayerPageImage;
#define groundLayerPage usampler2DArray(groundLayerPageImage, customSamplers[CS_NEAREST])

// The finest tile's texel, metres (0 without layers: everything is worked
// out where it is drawn).
float groundLayerFinest() { return grass.layer.w > 0.5 ? grass.layer2.w : 0.0; }

// Where a point is on the tiles: a flat body's x and z (one root), a round
// body's land's chart (its cube faces the roots, scene_land.glsl's
// landPosOf), in level-0 tiles of the turf's: the tile and the fraction.
LandPos groundPosOf(vec2 xz) {
    LandPos q;
    q.root = 0;
    vec2 t = xz / grass.layer.x;
    q.cell = ivec2(floor(t));
    q.frac = t - floor(t);
    return q;
}
// (`c` the point's chart place, landChartOf.)
LandPos groundChartPosOf(LandChart c) {
    int f = c.face;
    vec2 off = c.off;
    bool near = c.near;
    float g = grass.layer.x;
    LandPos q;
    q.root = f;
    vec2 t = off / g;
    if (near) {
        // The anchor's land tile in the turf's (a land tile is a power of
        // two of them: SceneMeshDrawer::groundLayers).
        vec4 a = landFaceAnchor(f);
        int k = int(LAND_W(LD_TILES / 4).x / g + 0.5);
        t += a.zw * float(k);
        vec2 w = floor(t);
        q.cell = ivec2(a.xy) * k + ivec2(w);
        q.frac = t - w;
        return q;
    }
    q.cell = ivec2(floor(t));
    q.frac = t - floor(t);
    return q;
}

// The slot of the tile of `level` holding `q`, and `uv` there; -1 when no
// tile there is resident.
int groundLayerSlot(LandPos q, int level, out vec2 uv) {
    ivec2 ti = q.cell >> level;
    vec2 t = (vec2(q.cell - (ti << level)) + q.frac) / exp2(float(level));
    int n = int(grass.layer2.z);
    uint e = texelFetch(groundLayerPage, ivec3(ti & (n - 1), q.root * int(grass.turf.w) + level), 0).r;
    uv = vec2(0.0);
    if (e == 0u) return -1;
    uvec2 hi = uvec2(ti >> int(grass.layer2.y)) & 1023u;
    if (((e >> 12u) & 1023u) != hi.x || (e >> 22u) != hi.y) return -1;
    float texels = grass.layer.y, border = grass.layer.z;
    uv = (border + t * texels) / (texels + 2.0 * border);
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

// The patterns at `q` for a footprint `fp` from the tiles: the level
// whose texel is the selection's most pixels at `fp` (SurfaceLayerConfig::
// texelPixels), blended toward the next coarser one across the level, or
// the finest coarser level resident when that one is not. False when no
// level is.
bool groundLayerSample(LandPos q, float fp, out GroundBake b) {
    b.a = b.b = b.c = b.d = vec4(0.0);
    int levels = int(grass.layer.w + 0.5);
    if (levels <= 0) return false;
    float lambda = clamp(log2(max(grass.layer2.x * fp / grass.layer2.w, 1e-6)), 0.0, float(levels - 1));
    int l0 = int(lambda);
    float f = lambda - float(l0);
    vec2 uv;
    int slot = -1, level = l0;
    for (; level < levels; ++level) {
        slot = groundLayerSlot(q, level, uv);
        if (slot >= 0) break;
    }
    if (slot < 0) return false;
    b = groundLayerFetch(uv, slot);
    if (level == l0 && f > 0.0 && l0 + 1 < levels) {
        vec2 uv1;
        int s1 = groundLayerSlot(q, l0 + 1, uv1);
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
