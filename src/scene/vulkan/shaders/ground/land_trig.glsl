// sin, tan and 1 - cos of the small angles a round body's land works in: a
// point metres from the anchor (or from a tile's middle) is an angle of a
// millionth of a radian at the Earth's radius, where the GPU's sin and cos
// are good to an absolute 1e-6 or so, not a relative one (NVIDIA's are the
// hardware's approximations): sin(th) R was metres off, in rings and spokes
// about the anchor, and a tile's height was read a metre from where it was
// drawn, the eroded ground's slopes a staircase of treads and risers. Under
// 0.02 rad (127 km at the Earth's radius) each is its series, exact in a
// float; past it the built-in, whose error is then a float's own.
//
// atan is never the built-in where it matters: Mesa's (RADV, lavapipe) is
// a polynomial good to a relative 2e-5 even at the smallest angles (its
// first coefficient is 0.99998), so R atan of a point 9.5 km out was 20 cm
// off, and off by another 20 cm in another anchor's frame: a re-anchor
// moved the turf (test_turf_body). NVIDIA's and Apple's are a float's own.
#ifndef LAND_TRIG_GLSL
#define LAND_TRIG_GLSL

const float LAND_TRIG_SMALL = 0.02;

float landSin(float x) {
    float x2 = x * x;
    return abs(x) < LAND_TRIG_SMALL ? x * (1.0 - x2 * (1.0 / 6.0) * (1.0 - x2 * (1.0 / 20.0))) : sin(x);
}
vec2 landSin(vec2 x) { return vec2(landSin(x.x), landSin(x.y)); }

float landTan(float x) {
    float x2 = x * x;
    return abs(x) < LAND_TRIG_SMALL ? x * (1.0 + x2 * (1.0 / 3.0 + x2 * (2.0 / 15.0))) : tan(x);
}
vec2 landTan(vec2 x) { return vec2(landTan(x.x), landTan(x.y)); }

// 1 - cos(x), as 2 sin^2(x / 2).
float landVersin(float x) {
    float s = landSin(0.5 * x);
    return 2.0 * s * s;
}

// atan(y / x), for x > 0 and |y| <= x, halved, as atan t = 2 atan(t / (1 +
// sqrt(1 + t^2))), until its series is exact in a float (three halvings at
// most, none under a tenth); elsewhere the built-in.
float landAtan(float y, float x) {
    if (!(x > 0.0) || abs(y) > x) return atan(y, x);
    float t = y / x, k = 1.0;
    while (abs(t) > 0.1) {
        t /= 1.0 + sqrt(1.0 + t * t);
        k *= 2.0;
    }
    float t2 = t * t;
    return k * t * (1.0 - t2 * (1.0 / 3.0 - t2 * (1.0 / 5.0 - t2 * (1.0 / 7.0 - t2 * (1.0 / 9.0)))));
}
vec2 landAtan(vec2 y, vec2 x) { return vec2(landAtan(y.x, x.x), landAtan(y.y, x.y)); }
vec2 landAtan(vec2 t) { return landAtan(t, vec2(1.0)); }
float landAtan(float t) { return landAtan(t, 1.0); }
#endif
