// sin, tan and 1 - cos of the small angles a round body's land works in: a
// point metres from the anchor (or from a tile's middle) is an angle of a
// millionth of a radian at the Earth's radius, where the GPU's sin and cos
// are good to an absolute 1e-6 or so, not a relative one (NVIDIA's are the
// hardware's approximations): sin(th) R was metres off, in rings and spokes
// about the anchor, and a tile's height was read a metre from where it was
// drawn, the eroded ground's slopes a staircase of treads and risers. Under
// 0.02 rad (127 km at the Earth's radius) each is its series, exact in a
// float; past it the built-in, whose error is then a float's own.
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

#endif
