#version 450
// Grass blades (scene_grass.glsl): one instance per cell of the draw's ring
// rectangle, the blade mesh bent into place by the wind and the movers. Drawn
// with mesh.frag's vegetation shading: the blade's normal leans toward the
// ground's, the sun glows through it from behind, and the root is in shade.
// shadow_grass.vert is the caster.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
layout(location = 7) out vec4 outVegShade;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_grass.glsl"

void main() {
    GrassBlade b;
    if (!grassBlade(gl_InstanceIndex, inPos, inNormal, b)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    outWorldPos = b.pos;
    gl_Position = camera.viewProj * vec4(b.pos, 1.0);
    outNormal = b.normal;
    outTangent = b.side;
    outBitangent = cross(b.normal, b.side);
    outUV = inUV;
    outColor = vec4(1.0);
    outInstColor = vec4(b.color, 0.0);
    // A blade: its ao, marked (w <= -2) so the fragment stage takes it for a
    // translucent blade with no paler underside; and the ground's normal.
    outVegShade = vec4(b.ground * grass.lush.w, -(2.0 + b.ao));
}
