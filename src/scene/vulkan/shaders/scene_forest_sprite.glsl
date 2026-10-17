// A value in [0, 1) from a 2D point (shadow_forest.frag's dither over a
// forest sprite's leaf-patch cells).

float forestSpriteHash(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}
