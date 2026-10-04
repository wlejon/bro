#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyePos;
    vec4 viewport;
    vec4 fogParams;
    vec4 fogColor;
} camera;

struct PointLight {
    vec4 position; // xyz = position, w = range
    vec4 color;    // rgb = color, a = intensity
};

layout(set = 1, binding = 0) uniform LightingUBO {
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 shadowSplits;
    mat4 shadowCascadeProj;
    vec4 numLights; // x: sun count, y: point light count, z: hasShadow, w: pad
    PointLight pointLights[16];
} lighting;

layout(set = 1, binding = 1) uniform sampler2DArrayShadow shadowMapArray;

layout(set = 2, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 1) uniform sampler2D texNormal;
layout(set = 2, binding = 2) uniform sampler2D texMetallicRoughness;
layout(set = 2, binding = 3) uniform sampler2D texEmissive;

layout(push_constant) uniform MeshPushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 emissive;
    vec4 pbrParams; // x: metallic, y: roughness, z: alphaCutoff, w: flags
} push;

const float PI = 3.14159265359;

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0000001);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float fogFactorFor(float camDist, float worldY) {
    float fogStart = camera.fogParams.x;
    float fogEnd = camera.fogParams.y;
    float fogDensity = camera.fogParams.z;
    float fogStartDist = camera.fogParams.w;
    float fogHeightFalloff = camera.fogColor.a;

    if (fogDensity > 0.0) {
        float d = max(camDist - fogStartDist, 0.0);
        float dens = fogDensity;
        if (fogHeightFalloff > 0.0) dens *= exp(-fogHeightFalloff * worldY);
        float x = dens * d;
        return 1.0 - exp(-x * x);
    }
    if (fogEnd > 0.0) {
        float f = clamp((camDist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
        return f * f;
    }
    return 0.0;
}

void main() {
    uint flags = uint(push.pbrParams.w);
    vec4 albedo = inColor;
    if ((flags & 1u) != 0u) {
        albedo *= texture(texAlbedo, inUV);
    }
    if (albedo.a < push.pbrParams.z) {
        discard;
    }

    vec3 N = normalize(inNormal);
    if ((flags & 2u) != 0u) {
        vec3 normalMap = texture(texNormal, inUV).rgb * 2.0 - 1.0;
        mat3 TBN = mat3(normalize(inTangent), normalize(inBitangent), N);
        N = normalize(TBN * normalMap);
    }

    float metallic = push.pbrParams.x;
    float roughness = push.pbrParams.y;
    if ((flags & 4u) != 0u) {
        vec4 mr = texture(texMetallicRoughness, inUV);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    vec3 V = normalize(camera.eyePos.xyz - inWorldPos);
    vec3 L = normalize(-lighting.sunDirection.xyz);
    vec3 H = normalize(V + L);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo.rgb, metallic);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    vec3 diffuse = (kD * albedo.rgb) / PI;

    // Shadow cascade factor
    float shadow = 1.0;
    if (lighting.numLights.z > 0.5) {
        vec4 shadowCoord = lighting.shadowCascadeProj * vec4(inWorldPos, 1.0);
        if (shadowCoord.w > 0.0) {
            vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
            projCoords.xy = projCoords.xy * 0.5 + 0.5;
            if (projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
                projCoords.y >= 0.0 && projCoords.y <= 1.0 &&
                projCoords.z >= 0.0 && projCoords.z <= 1.0) {
                shadow = texture(shadowMapArray, vec4(projCoords.xy, 0.0, projCoords.z - 0.002));
            }
        }
    }

    vec3 direct = vec3(0.0);
    if (lighting.numLights.x > 0.5) {
        vec3 radiance = lighting.sunColor.rgb * lighting.sunColor.a;
        direct += (diffuse + specular) * radiance * NdotL * shadow;
    }

    int pointCount = int(lighting.numLights.y);
    for (int i = 0; i < pointCount && i < 16; ++i) {
        vec3 toLight = lighting.pointLights[i].position.xyz - inWorldPos;
        float d = length(toLight);
        if (d > 1e-4) {
            vec3 pL = toLight / d;
            float range = lighting.pointLights[i].position.w;
            float atten = 1.0;
            if (range > 0.0) {
                float t = d / range;
                float t4 = t * t * t * t;
                float win = clamp(1.0 - t4, 0.0, 1.0);
                win = win * win;
                atten = win / (d * d + 1.0);
            }
            float pNdotL = max(dot(N, pL), 0.0);
            if (pNdotL > 0.0 && atten > 0.0) {
                vec3 pH = normalize(V + pL);
                float pNDF = distributionGGX(N, pH, roughness);
                float pG = geometrySmith(N, V, pL, roughness);
                vec3 pF = fresnelSchlick(max(dot(pH, V), 0.0), F0);
                vec3 pSpec = (pNDF * pG * pF) / (4.0 * NdotV * pNdotL + 0.0001);
                vec3 pkD = (vec3(1.0) - pF) * (1.0 - metallic);
                vec3 pDiff = (pkD * albedo.rgb) / PI;
                vec3 pRadiance = lighting.pointLights[i].color.rgb * (lighting.pointLights[i].color.a * atten);
                direct += (pDiff + pSpec) * pRadiance * pNdotL;
            }
        }
    }

    // Ambient lighting
    vec3 ambient = lighting.ambientColor.rgb * lighting.ambientColor.a * albedo.rgb;

    // Emissive
    vec3 emissive = push.emissive.rgb * push.emissive.a;
    if ((flags & 8u) != 0u) {
        emissive *= texture(texEmissive, inUV).rgb;
    }

    vec3 color = ambient + direct + emissive;
    if ((flags & 16u) != 0u) {
        color = albedo.rgb + emissive;
    }

    float camDist = length(camera.eyePos.xyz - inWorldPos);
    float fogFactor = fogFactorFor(camDist, inWorldPos.y);
    if (fogFactor > 0.0) {
        color = mix(color, camera.fogColor.rgb, fogFactor);
    }

    outColor = vec4(color, albedo.a);
}
