#include "scene/impostor_layer.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include <bromesh/mesh_data.h>
#include <cmath>
#include <algorithm>

namespace bro::scene {

static const char* kImpostorVertexShader = R"(
uniform vec2  u_grid;
uniform float u_half;
uniform vec2  u_cull;

flat out vec2 v_uvMin;
flat out vec2 v_uvMax;
out float v_fade;

void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
    float scl = normal.x;
    vec2 corner = uv;

    vec3 centerCR = (uModel * vec4(pos, 1.0)).xyz;
    float camDist = length(centerCR);
    v_fade = 1.0 - smoothstep(u_cull.x, u_cull.y, camDist);

    vec3 f = camDist > 1e-4 ? centerCR / camDist : vec3(0.0, 0.0, 1.0);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), f);
    float rl = length(right);
    right = rl > 1e-4 ? right / rl : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(f, right);

    vec3 offset = right * (corner.x * u_half * scl) + up * (corner.y * u_half * scl);
    pos = pos + offset;
    normal = -f;

    vec3 dir = -f;
    vec3 ad = abs(dir);
    vec3 d = dir / (ad.x + ad.y + ad.z);
    float coordX = d.x + d.z;
    float coordY = d.x - d.z;
    float col = floor(clamp(coordX * 0.5 + 0.5, 0.0, 0.999999) * u_grid.x);
    float row = floor(clamp(coordY * 0.5 + 0.5, 0.0, 0.999999) * u_grid.y);
    vec2 cellSz = vec2(1.0 / u_grid.x, 1.0 / u_grid.y);
    v_uvMin = vec2(col, row) * cellSz;
    v_uvMax = v_uvMin + cellSz;

    uv = corner * 0.5 + 0.5;

    if (v_fade <= 0.001) pos = pos - offset;
}
)";

static const char* kImpostorFragmentShader = R"(
flat in vec2 v_uvMin;
flat in vec2 v_uvMax;
in float v_fade;

void userFragment(inout vec3 baseColor, inout vec3 normal,
                  inout float metallic, inout float roughness,
                  inout vec3 emissive, inout float alpha) {
    vec2 cellUV = vec2(vUV.x, 1.0 - vUV.y);
    vec2 uv = v_uvMin + cellUV * (v_uvMax - v_uvMin);
    vec4 tex = texture(uBaseColorTex, uv);
    if (tex.a < 0.5) discard;

    if (v_fade < 0.999) {
        float hash = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        if (hash > v_fade) discard;
    }
    baseColor = vec3(0.0);
    emissive = tex.rgb;
    alpha = 1.0;
}
)";

ImpostorLayerResult createImpostorLayer(
    SceneGraph* scene,
    const ImpostorAtlasInfo& atlas,
    const float* transforms,
    size_t transformFloatCount,
    const ImpostorOptions& opts)
{
    ImpostorLayerResult res;
    if (!scene || !transforms || transformFloatCount < 9) return res;

    int count = static_cast<int>(transformFloatCount / 9);
    res.quadCount = count;

    float halfExt = std::max(atlas.boundsRadius, 1e-3f) * opts.margin;

    bromesh::MeshData meshData;
    meshData.positions.resize(count * 4 * 3);
    meshData.normals.resize(count * 4 * 3);
    meshData.uvs.resize(count * 4 * 2);
    meshData.indices.resize(count * 6);

    const float corners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};

    for (int i = 0; i < count; i++) {
        size_t o = i * 9;
        float scl = transforms[o + 7];
        float cxw = transforms[o] + atlas.boundsCenter.x * scl;
        float cyw = transforms[o + 1] + atlas.boundsCenter.y * scl;
        float czw = transforms[o + 2] + atlas.boundsCenter.z * scl;

        for (int k = 0; k < 4; k++) {
            size_t v = i * 4 + k;
            meshData.positions[v * 3] = cxw;
            meshData.positions[v * 3 + 1] = cyw;
            meshData.positions[v * 3 + 2] = czw;

            meshData.normals[v * 3] = scl;
            meshData.normals[v * 3 + 1] = 0.0f;
            meshData.normals[v * 3 + 2] = 0.0f;

            meshData.uvs[v * 2] = corners[k][0];
            meshData.uvs[v * 2 + 1] = corners[k][1];
        }

        uint32_t b = i * 4;
        size_t t = i * 6;
        meshData.indices[t] = b;
        meshData.indices[t + 1] = b + 1;
        meshData.indices[t + 2] = b + 2;
        meshData.indices[t + 3] = b;
        meshData.indices[t + 4] = b + 2;
        meshData.indices[t + 5] = b + 3;
    }

    MeshNode* node = scene->createMesh();
    if (!node) return res;

    node->setMesh(std::move(meshData));
    node->setTwoSided(true);
    node->setCastsShadow(false);
    node->setReceivesShadow(false);

    if (atlas.width > 0 && atlas.height > 0 && !atlas.textureData.empty()) {
        std::vector<float> floatTex(atlas.textureData.size());
        for (size_t i = 0; i < atlas.textureData.size(); i++) {
            floatTex[i] = atlas.textureData[i] / 255.0f;
        }
        node->setCustomShaderTexture("uBaseColorTex", atlas.width, atlas.height, floatTex.data(), false, false, false, 4);
    }

    node->setCustomShader(kImpostorVertexShader, kImpostorFragmentShader);

    float grid[2] = {static_cast<float>(atlas.cols), static_cast<float>(atlas.rows)};
    node->setCustomShaderUniform("u_grid", 2, grid);
    node->setCustomShaderUniform("u_half", 1, &halfExt);

    float cull[2] = {opts.cullNear, opts.cullFar};
    node->setCustomShaderUniform("u_cull", 2, cull);

    node->setCullMargin(halfExt * 2.0f);

    res.node = node;
    return res;
}

} // namespace bro::scene
