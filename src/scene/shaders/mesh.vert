// vWorldPos is in camera-relative space (uModel has eye pre-subtracted).
// That keeps precision at planet scale and means uCameraPos == 0, which
// simplifies the view-vector math in the fragment shader.

#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;   // xyz = tangent, w = handedness

uniform mat4 uMVP;
uniform mat4 uModel;
uniform int uUseVertexColor;
uniform vec3  uWindDir;
uniform float uWindStrength;
uniform float uWindTime;
uniform float uWindFreq;
uniform float uWindMask;   // per-mesh opt-in (0 = static, 1 = sway). Multiplied
                           // into vertex-color R so flora opts in but terrain
                           // (whose color R is per-vertex shade) stays still.

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vCamDist;
out vec3 vTangentW;
out vec3 vBitangentW;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    mat3 M3 = mat3(uModel);
    vec3 swayedAPos = aPos;
    if (uWindStrength > 0.0 && uWindMask > 0.0) {
        float bend  = aColor.r * uWindMask;
        float phase = sin(uWindTime * uWindFreq
                          + dot(worldPos.xz, vec2(0.3, 0.5)));
        vec3 deltaWorld = uWindDir * (phase * uWindStrength * bend);
        // Push the world delta back into object space (orthonormal-rotation
        // approximation of inverse(M3)) so the same uMVP can transform it.
        vec3 deltaObj   = transpose(M3) * deltaWorld;
        swayedAPos += deltaObj;
        worldPos.xyz += deltaWorld;
    }
    vWorldPos = worldPos.xyz;
    vNormal = M3 * aNormal;
    vTangentW   = M3 * aTangent.xyz;
    vBitangentW = cross(vNormal, vTangentW) * aTangent.w;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(worldPos.xyz);
    gl_Position = uMVP * vec4(swayedAPos, 1.0);
}
