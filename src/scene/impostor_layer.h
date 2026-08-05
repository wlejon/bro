#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <bromath/vec.h>

namespace bro::scene {

class SceneGraph;
class MeshNode;

struct ImpostorAtlasInfo {
    int width = 0;
    int height = 0;
    int cols = 1;
    int rows = 1;
    bromath::Vec3 boundsCenter{0.0f, 0.0f, 0.0f};
    float boundsRadius = 1.0f;
    std::vector<uint8_t> textureData;
};

struct ImpostorOptions {
    float margin = 1.03f;
    float cullNear = 450.0f;
    float cullFar = 950.0f;
};

struct ImpostorLayerResult {
    MeshNode* node = nullptr;
    int quadCount = 0;
};

ImpostorLayerResult createImpostorLayer(
    SceneGraph* scene,
    const ImpostorAtlasInfo& atlas,
    const float* transforms,
    size_t transformFloatCount,
    const ImpostorOptions& opts = {}
);

} // namespace bro::scene
