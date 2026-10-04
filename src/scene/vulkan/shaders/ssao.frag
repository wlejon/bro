#version 450
#define KERNEL_SIZE 16

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepthTex;
layout(set = 0, binding = 1) uniform sampler2D uNoiseTex;

layout(set = 0, binding = 2) uniform SSAOUBO {
    mat4 uProj;
    mat4 uInvProj;
    vec4 uKernel[KERNEL_SIZE];
    vec4 uParams; // x: radius, y: bias, z: noiseScaleX, w: noiseScaleY
} ubo;

vec3 viewPos(vec2 uv) {
    float d = texture(uDepthTex, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, d, 1.0);
    vec4 v = ubo.uInvProj * clip;
    return v.xyz / v.w;
}

void main() {
    float d0 = texture(uDepthTex, inUV).r;
    if (d0 <= 0.0) {
        outColor = vec4(1.0);
        return;
    }

    vec3 P = viewPos(inUV);
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));
    if (N.z < 0.0) N = -N;

    vec2 noiseScale = ubo.uParams.zw;
    vec3 rnd = vec3(texture(uNoiseTex, inUV * noiseScale).xy * 2.0 - 1.0, 0.0);
    vec3 T = normalize(rnd - N * dot(rnd, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float radius = ubo.uParams.x;
    float bias = ubo.uParams.y;

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; ++i) {
        vec3 sp = P + TBN * ubo.uKernel[i].xyz * radius;
        vec4 off = ubo.uProj * vec4(sp, 1.0);
        off.xyz /= off.w;
        vec2 suv = off.xy * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
        float sampleZ = viewPos(suv).z;
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(P.z - sampleZ));
        occlusion += (sampleZ >= sp.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - occlusion / float(KERNEL_SIZE);
    outColor = vec4(ao, ao, ao, 1.0);
}
