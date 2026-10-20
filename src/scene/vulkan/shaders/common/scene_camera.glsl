// Set 0 of every lit scene pipeline: the camera (SceneCameraUniforms,
// scene_vk_descriptors.h). Included by the mesh, shadow and terrain shaders;
// the other passes declare the prefix they read.
//
// Everything is CAMERA-RELATIVE (scene/vulkan/frame/scene_view.h): model matrices
// arrive with the eye subtracted, so a "world" position in any scene shader
// is world - eye, the eye sits at the origin, and view / viewProj take such
// positions. eyeWorld is the one absolute position, for what is a function of
// where the camera really is (height fog, the atmosphere).
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyeWorld;
    vec4 viewport;    // x: width, y: height, z: near, w: far
    vec4 fogParams;   // x: start, y: end, z: density, w: start distance
    vec4 fogColor;    // rgb, a: height falloff
    vec4 wind;        // xyz: direction, w: strength
    vec4 windParams;  // x: time (s), y: frequency, z: the eye's phase (dot(eye.xz, (0.3, 0.5)) mod 2 pi),
                      // w: the oscillator clock, time mod SCENE_WIND_CLOCK_PERIOD (reduced in double)
    // The wind field (scene/world/wind_field.h): what vegetation bends with.
    vec4 windField;   // xy: horizontal direction (x, z; unit, drifting), z: steady speed (m/s), w: gust travel speed (m/s)
    vec4 windGust;    // x: gust amount, y: 1 / band length along the wind, z: 1 / across it, w: 0
    // The plant movers (scene/nodes/plant_movers.h; scene_vegetation.glsl's vegMoverPush).
    vec4 plantMoverInfo;   // x: movers, y: trail samples a mover
    vec4 plantMovers[8];   // two a mover: (camera-relative x, z, its trail's reach, radius), (height, strength, 0, 0)
    vec4 plantTrail[80];   // y samples a mover, oldest first: (camera-relative x, z, age (s), window)
} camera;

// The engine's wind uniforms under the names custom shader chunks use.
#define uWindDir (camera.wind.xyz)
#define uWindStrength (camera.wind.w)
#define uWindTime (camera.windParams.x)
#define uWindFreq (camera.windParams.y)

// Wind sway in world space for a vertex at camera-relative `pos` whose bend
// weight is `bend` (vertex colour R times the mesh's wind mask). Zero when
// calm. The phase is that of the absolute position: the eye's share comes
// reduced from the CPU.
vec3 windDelta(vec3 pos, float bend) {
    if (camera.wind.w <= 0.0 || bend <= 0.0) return vec3(0.0);
    float phase = sin(camera.windParams.x * camera.windParams.y + camera.windParams.z + dot(pos.xz, vec2(0.3, 0.5)));
    return camera.wind.xyz * (phase * camera.wind.w * bend);
}

// --- The wind field ------------------------------------------------------------
// The GPU half of scene/world/wind_field.h (scene.windAt() is the CPU half): keep
// the two in step. Positions are absolute world here.

float sceneWindHash(ivec2 p) {
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) / 4294967296.0;
}

// Value noise in [-1, 1].
float sceneWindNoise(vec2 p) {
    vec2 i = floor(p), f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    ivec2 ii = ivec2(i);
    float a = sceneWindHash(ii), b = sceneWindHash(ii + ivec2(1, 0));
    float c = sceneWindHash(ii + ivec2(0, 1)), d = sceneWindHash(ii + ivec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 2.0 - 1.0;
}

// Simplex gradient noise, about [-0.8, 0.8] (its spread is the value
// noise's: standard deviation 0.43): the gusts and the sward's comb, the
// fields seen whole from above as the sward's sheen. Value noise is flat at
// every lattice point, so a band cut from it (the gusts' soft ramp) traces
// the lattice: rounded rectangles in rows, staircase edges. A gradient
// noise on the simplex grid has no plateaus and no square axes, so its
// bands are the wind's shapes. Each corner's (0.5 - r^2)^4 falls off smooth
// to its second derivative: no jerk in a limb a band crosses. Its grid is
// SCENE_WIND_SIMPLEX_GRID of the value noise's period, so a feature is as
// wide as one of the value noise's (correlation 0.5 about 0.6 apart).
const float SCENE_WIND_SIMPLEX_GRID = 0.38;
float sceneWindSimplexCorner(ivec2 c, vec2 d) {
    float t = 0.5 - dot(d, d);
    if (t <= 0.0) return 0.0;
    float a = sceneWindHash(c) * 6.2831853;
    t *= t;
    return t * t * (cos(a) * d.x + sin(a) * d.y);
}
float sceneWindSimplex(vec2 p) {
    const float F2 = 0.36602540378, G2 = 0.21132486540;
    p *= SCENE_WIND_SIMPLEX_GRID;
    vec2 i = floor(p + (p.x + p.y) * F2);
    vec2 d0 = p - i + (i.x + i.y) * G2;
    ivec2 o = d0.x > d0.y ? ivec2(1, 0) : ivec2(0, 1);
    vec2 d1 = d0 - vec2(o) + G2;
    vec2 d2 = d0 - 1.0 + 2.0 * G2;
    ivec2 ii = ivec2(i);
    return 80.0 * (sceneWindSimplexCorner(ii, d0) + sceneWindSimplexCorner(ii + o, d1) +
                   sceneWindSimplexCorner(ii + ivec2(1), d2));
}

// max(x, 0) with its corner rounded (a hyperbola, smooth to every
// derivative, within SCENE_GUST_SOFT of the corner): a band's edge starts a
// gust with no kink in what it drives's speed and no jump in its
// acceleration.
const float SCENE_GUST_SOFT = 0.06;
float sceneWindSoftRamp(float x) { return 0.5 * (x + sqrt(x * x + SCENE_GUST_SOFT * SCENE_GUST_SOFT)); }

// The gust at absolute world xz at time t: 0 between the bands, up to ~1.7 x
// the gust amount inside one. The bands travel downwind at windField.w.
// `fine` adds the second octave; without it the gust is its slow part.
float sceneWindGustAtTime(vec2 xz, float t, bool fine) {
    if (camera.windGust.x <= 0.0) return 0.0;
    vec2 d = camera.windField.xy;
    float along = dot(xz, d) - t * camera.windField.w;
    float across = dot(xz, vec2(-d.y, d.x));
    vec2 k = camera.windGust.yz;
    float g = sceneWindSimplex(vec2(along * k.x, across * k.y)) * 0.5 + 0.5;
    if (fine) g += 0.35 * sceneWindSimplex(vec2(along * k.x * 3.18 + 5.3, across * k.y * 4.17));
    return sceneWindSoftRamp(g - 0.45) * 2.4 * camera.windGust.x;
}

// The oscillator clock. Everything that sways or flutters at a frequency
// runs on the wind clock reduced modulo SCENE_WIND_CLOCK_PERIOD (in double,
// on the CPU: windParams.w), with its frequency rounded to a whole number
// of cycles in that period, so its phase is exact after hours of play and
// carries on unbroken through the wrap. A frequency must be fixed: one that
// varies multiplies the clock into a chirp (its instantaneous frequency
// grows with the time), so the wind changes how far a thing moves, never
// how fast it cycles.
const float SCENE_WIND_CLOCK_PERIOD = 256.0;
// The cycles (fraction in [0, 1)) of an oscillator of `hz` now.
float sceneWindCycles(float hz) {
    float f = floor(hz * SCENE_WIND_CLOCK_PERIOD + 0.5) / SCENE_WIND_CLOCK_PERIOD;
    return fract(f * camera.windParams.w);
}

// The gust at absolute world xz now (wind_field.h's gustAt).
float sceneWindGustAt(vec2 xz) { return sceneWindGustAtTime(xz, camera.windParams.x, true); }

// The wind at camera-relative `pos`: xyz the velocity (m/s, horizontal), w
// the gust there.
vec4 sceneWind(vec3 pos) {
    vec2 xz = pos.xz + camera.eyeWorld.xz;
    float g = sceneWindGustAt(xz);
    vec2 d = camera.windField.xy;
    float spd = camera.windField.z + g * 4.0;
    float t = camera.windParams.x;
    float wobble = sceneWindNoise(vec2(dot(xz, d) * 0.03 - t * 0.3, t * 0.1)) * 0.25;
    vec2 v = (d + vec2(-d.y, d.x) * wobble) * spd;
    return vec4(v.x, 0.0, v.y, g);
}
