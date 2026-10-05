#version 450

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uColorTex;
layout(set = 0, binding = 1) uniform sampler2D uDepthTex;

layout(set = 0, binding = 2) uniform SSRUBO {
    mat4 uProj;
    mat4 uInvProj;
    vec4 uParams1; // x: isPerspective, y: maxDist, z: steps, w: thickness
    vec4 uParams2; // x: intensity, y: edgeFade, zw: pad
} ubo;

bool isSky(float d) { return REVERSED_Z ? d <= 0.0 : d >= 1.0; }

vec3 viewPos(vec2 uv, float d) {
    vec4 clip = vec4(uv * 2.0 - 1.0, d, 1.0);
    vec4 v = ubo.uInvProj * clip;
    return v.xyz / v.w;
}

void main() {
    vec4 src = texture(uColorTex, inUV);
    float d0 = texture(uDepthTex, inUV).r;
    if (isSky(d0)) discard;

    vec3 P = viewPos(inUV, d0);
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));
    if (N.z < 0.0) N = -N;

    float mask = src.a;
    float intensity = ubo.uParams2.x;
    float edgeFade = ubo.uParams2.y;
    int isPerspective = int(ubo.uParams1.x + 0.5);
    float maxDist = ubo.uParams1.y;
    int steps = int(ubo.uParams1.z + 0.5);
    float thickness = ubo.uParams1.w;

    vec3 refl = vec3(0.0);
    float w = 0.0;

    if (mask * intensity > 0.002) {
        vec3 I = (isPerspective == 1) ? normalize(P) : vec3(0.0, 0.0, -1.0);
        vec3 R = reflect(I, N);
        float facingFade = 1.0 - smoothstep(0.35, 0.9, dot(R, -I));

        if (facingFade > 0.001) {
            float stepLen = maxDist / float(steps);
            vec3 O = P + N * min(0.05, stepLen * 0.5);

            float tPrev = 0.0;
            float dzPrev = -1.0;
            float t = stepLen;
            for (int i = 0; i < steps; ++i) {
                vec3 Q = O + R * t;
                vec4 clip = ubo.uProj * vec4(Q, 1.0);
                if (isPerspective == 1 && clip.w <= 0.0) break;
                vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
                if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;

                float sd = texture(uDepthTex, uv).r;
                float dz = !isSky(sd) ? viewPos(uv, sd).z - Q.z : -1.0;

                if (dz > 0.0 && dzPrev <= 0.0) {
                    float lo = tPrev, hi = t;
                    vec2 hitUV = uv;
                    float hitDz = dz;
                    for (int j = 0; j < 6; ++j) {
                        float mid = 0.5 * (lo + hi);
                        vec3 Qm = O + R * mid;
                        vec4 cm = ubo.uProj * vec4(Qm, 1.0);
                        vec2 um = (cm.xy / cm.w) * 0.5 + 0.5;
                        float sm = texture(uDepthTex, um).r;
                        float dm = !isSky(sm) ? viewPos(um, sm).z - Qm.z : -1.0;
                        if (dm > 0.0) { hi = mid; hitUV = um; hitDz = dm; }
                        else          { lo = mid; }
                    }
                    if (hitDz <= thickness) {
                        vec2 b = min(hitUV, 1.0 - hitUV);
                        float edge = (edgeFade > 0.0)
                            ? smoothstep(0.0, edgeFade, min(b.x, b.y))
                            : 1.0;
                        refl = texture(uColorTex, hitUV).rgb;
                        w = clamp(mask * intensity, 0.0, 1.0) * edge * facingFade;
                        break;
                    }
                }
                dzPrev = dz;
                tPrev = t;
                t += stepLen;
            }
        }
    }

    outColor = vec4(mix(src.rgb, refl, w), 1.0);
}
