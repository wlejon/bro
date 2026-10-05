// Set 0 of every lit scene pipeline: the camera (SceneCameraUniforms,
// scene_vk_descriptors.h). Included by the mesh, shadow and terrain shaders;
// the other passes declare the prefix they read.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyePos;
    vec4 viewport;    // x: width, y: height, z: near, w: far
    vec4 fogParams;   // x: start, y: end, z: density, w: start distance
    vec4 fogColor;    // rgb, a: height falloff
    vec4 wind;        // xyz: direction, w: strength
    vec4 windParams;  // x: time (s), y: frequency
} camera;

// The engine's wind uniforms under the names custom shader chunks use.
#define uWindDir (camera.wind.xyz)
#define uWindStrength (camera.wind.w)
#define uWindTime (camera.windParams.x)
#define uWindFreq (camera.windParams.y)

// Wind sway in world space for a vertex at `worldPos` whose bend weight is
// `bend` (vertex colour R times the mesh's wind mask). Zero when calm.
vec3 windDelta(vec3 worldPos, float bend) {
    if (camera.wind.w <= 0.0 || bend <= 0.0) return vec3(0.0);
    float phase = sin(camera.windParams.x * camera.windParams.y + dot(worldPos.xz, vec2(0.3, 0.5)));
    return camera.wind.xyz * (phase * camera.wind.w * bend);
}
