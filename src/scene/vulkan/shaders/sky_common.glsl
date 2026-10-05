// What the three sky fragment shaders share: the view direction from sky.vert,
// the colour and indirect outputs, the camera and lighting sets (the
// atmosphere and the environment cube live in the lighting block) and the
// push block of scene/vulkan/scene_environment.cpp.

layout(location = 0) in vec3 inWorldDir;
layout(location = 0) out vec4 outColor;
// The opaque scope's indirect-light attachment, when SSAO adds one: the sky
// has no indirect light for AO to take away. Without it the write goes nowhere.
layout(location = 1) out vec4 outIndirect;

#include "scene_camera.glsl"
#include "scene_lighting.glsl"

layout(push_constant) uniform SkyPush {
    vec4 params;   // starfield: x = intensity, y = density, z = rotation
} sky;
