// A cutout caster (PassShadow's SHADOW_CUTOUT pipelines): a textured mesh
// with an alpha cutoff casts what the colour pass keeps, its base colour's
// alpha (mesh.frag's: the texture's at albedoUV, by the base colour's and
// the vertex colour's) against the cutoff. The caster's push carries them
// in params.yzw: y the cutoff, z
// the base colour's alpha (negated when the vertex colour's alpha counts
// too), w the atlas grid (cols + rows * 256, mesh.frag's push.extra.w).

#ifdef SHADOW_CUTOUT_VERTEX
layout(location = 0) out vec2 outCutUV;
layout(location = 1) out float outCutAlpha;
layout(location = 2) flat out vec4 outCut;   // x cutoff, y atlas cell, zw grid cols, rows

// `uv` after the custom chunk, `vertexAlpha` the vertex colour's, `cellCode`
// an instance record's alpha (1 for a mesh's own: mesh.vert's outInstColor).
void shadowCutoutOut(vec2 uv, float vertexAlpha, float cellCode) {
    float base = push.params.z;
    outCutUV = uv;
    outCutAlpha = base < 0.0 ? -base * vertexAlpha : base;
    float grid = push.params.w;
    float cols = max(floor(mod(grid, 256.0)), 1.0), rows = max(floor(grid / 256.0), 1.0);
    outCut = vec4(push.params.y, cellCode, cols, rows);
}
#endif

#ifdef SHADOW_CUTOUT_FRAGMENT
layout(set = 2, binding = 0) uniform sampler2D texAlbedo;
layout(location = 0) in vec2 inCutUV;
layout(location = 1) in float inCutAlpha;
layout(location = 2) flat in vec4 inCut;

void shadowCutoutTest() {
    vec2 uv = inCutUV;
    int cols = int(inCut.z), rows = int(inCut.w);
    if (cols != 1 || rows != 1) {
        int cell = clamp(int(inCut.y * 256.0), 0, cols * rows - 1);
        vec2 cellSize = vec2(1.0 / float(cols), 1.0 / float(rows));
        uv = (vec2(float(cell % cols), float(cell / cols)) + fract(inCutUV)) * cellSize;
    }
    if (texture(texAlbedo, uv).a * inCutAlpha < inCut.x) discard;
}
#endif
