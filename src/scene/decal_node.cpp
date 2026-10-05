#include "scene/decal_node.h"

namespace bro::scene {

DecalNode::DecalNode(const std::string& name) : SceneNode(name) {}

void DecalNode::setAlbedoTexture(int width, int height, const uint8_t* rgba) {
    albedoTex_.set(width, height, rgba);
    bumpChangeGeneration();
}

void DecalNode::clearAlbedoTexture() {
    albedoTex_.clear();
    bumpChangeGeneration();
}

void DecalNode::setEmissionTexture(int width, int height, const uint8_t* rgba) {
    emissionTex_.set(width, height, rgba);
    bumpChangeGeneration();
}

void DecalNode::clearEmissionTexture() {
    emissionTex_.clear();
    bumpChangeGeneration();
}

} // namespace bro::scene
