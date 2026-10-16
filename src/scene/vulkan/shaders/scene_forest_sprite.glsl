// The cutout of a forest sprite (mesh_forest_sprite.vert): a disc with a
// ragged, leafy edge and a few gaps toward its rim, so many overlapping
// sprites read as foliage rather than as balls. `uv` runs -1 .. 1 across the
// sprite, `seed` in [0, 1) varies it. Shared by mesh.frag and
// shadow_forest.frag, so a sprite's shadow has its outline.

float forestSpriteHash(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

float forestSpriteNoise(vec2 p) {
    vec2 i = floor(p), f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = forestSpriteHash(i), b = forestSpriteHash(i + vec2(1.0, 0.0));
    float c = forestSpriteHash(i + vec2(0.0, 1.0)), d = forestSpriteHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// The light inside a clump's disc at leaf scale: the leaves its lobe holds
// (about ten across), each a little lighter or darker, darker where they
// part. A multiplier on the colour, 0.7 .. 1.1.
float forestSpriteLeafShade(vec2 uv, float seed) {
    vec2 o = vec2(seed * 61.0, seed * 29.0);
    float n = forestSpriteNoise(uv * 5.5 + o) * 0.6 + forestSpriteNoise(uv * 11.0 + o.yx) * 0.4;
    return 0.7 + 0.4 * n;
}

bool forestSpriteCovers(vec2 uv, float seed) {
    float r = length(uv);
    vec2 o = vec2(seed * 97.0, seed * 53.0);
    float edge = 0.62 + 0.38 * forestSpriteNoise(uv * 3.5 + o) + 0.1 * forestSpriteNoise(uv * 9.0 + o.yx);
    if (r > edge) return false;
    // Gaps where the leaves part, more of them toward the rim.
    return forestSpriteNoise(uv * 7.0 + o * 1.7) > 0.32 * smoothstep(0.35, 1.0, r);
}
