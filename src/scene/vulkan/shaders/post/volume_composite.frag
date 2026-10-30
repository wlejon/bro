#version 450
// The volumes' reconstructed result (PassVolume) over the HDR scope: the
// pipeline blends it as scene * transmittance (alpha) + light (rgb).

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uVolumes;

void main() {
    vec4 v = texelFetch(uVolumes, ivec2(gl_FragCoord.xy), 0);
    if (v.a >= 1.0 && max(v.r, max(v.g, v.b)) <= 0.0) discard;
    outColor = vec4(v.rgb, clamp(v.a, 0.0, 1.0));
}
