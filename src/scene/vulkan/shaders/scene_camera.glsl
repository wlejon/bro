// Set 0 of every lit scene pipeline: the camera (SceneCameraUniforms,
// scene_vk_descriptors.h). Included by the mesh, shadow and terrain shaders;
// the other passes declare the prefix they read.
//
// Everything is CAMERA-RELATIVE (scene/vulkan/scene_view.h): model matrices
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
    vec4 windParams;  // x: time (s), y: frequency, z: the eye's phase (dot(eye.xz, (0.3, 0.5)) mod 2 pi)
    // The wind field (scene/wind_field.h): what vegetation bends with.
    vec4 windField;   // xy: horizontal direction (x, z; unit, drifting), z: steady speed (m/s), w: gust travel speed (m/s)
    vec4 windGust;    // x: gust amount, y: 1 / band length along the wind, z: 1 / across it, w: 0
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
// The GPU half of scene/wind_field.h (scene.windAt() is the CPU half): keep
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

// The gust at absolute world xz: 0 between the bands, up to ~2 x the gust
// amount inside one. The bands travel downwind at windField.w.
float sceneWindGustAt(vec2 xz) {
    if (camera.windGust.x <= 0.0) return 0.0;
    vec2 d = camera.windField.xy;
    float along = dot(xz, d) - camera.windParams.x * camera.windField.w;
    float across = dot(xz, vec2(-d.y, d.x));
    vec2 k = camera.windGust.yz;
    float g = sceneWindNoise(vec2(along * k.x, across * k.y)) * 0.5 + 0.5 +
              0.35 * sceneWindNoise(vec2(along * k.x * 3.18 + 5.3, across * k.y * 4.17));
    return max(g - 0.45, 0.0) * 2.4 * camera.windGust.x;
}

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
