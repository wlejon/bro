#include "scene/decal_node.h"

namespace bro::scene {

DecalNode::DecalNode(const std::string& name) : SceneNode(name) {}

DecalNode::~DecalNode() {
    releaseGL();
}

void DecalNode::setAlbedoTexture(int width, int height, const uint8_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba) {
        clearAlbedoTexture();
        return;
    }
    pendingAlbedo_.data.assign(rgba, rgba + (size_t)width * (size_t)height * 4);
    pendingAlbedo_.w = width;
    pendingAlbedo_.h = height;
    pendingAlbedo_.dirty = true;
    hasAlbedo_ = true;
    bumpChangeGeneration();
}

void DecalNode::clearAlbedoTexture() {
    pendingAlbedo_.data.clear();
    pendingAlbedo_.w = pendingAlbedo_.h = 0;
    pendingAlbedo_.dirty = true;   // flush deletes the GL texture
    hasAlbedo_ = false;
    bumpChangeGeneration();
}

void DecalNode::setEmissionTexture(int width, int height, const uint8_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba) {
        clearEmissionTexture();
        return;
    }
    pendingEmission_.data.assign(rgba, rgba + (size_t)width * (size_t)height * 4);
    pendingEmission_.w = width;
    pendingEmission_.h = height;
    pendingEmission_.dirty = true;
    hasEmission_ = true;
    bumpChangeGeneration();
}

void DecalNode::clearEmissionTexture() {
    pendingEmission_.data.clear();
    pendingEmission_.w = pendingEmission_.h = 0;
    pendingEmission_.dirty = true;
    hasEmission_ = false;
    bumpChangeGeneration();
}

// Upload one staged slot: empty data deletes the texture, otherwise
// (re)create with mipmaps. CLAMP_TO_EDGE — projection UVs cover [0,1]
// exactly inside the box, so repeat wrap would bleed opposite edges into
// the border filtering.
void DecalNode::flushSlot(PendingTex& slot, GLuint& tex) {
    if (!slot.dirty) return;
    slot.dirty = false;
    if (slot.data.empty()) {
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        return;
    }
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, slot.w, slot.h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, slot.data.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    slot.data.clear();
    slot.data.shrink_to_fit();
}

void DecalNode::flushPendingTextures() {
    flushSlot(pendingAlbedo_, albedoTex_);
    flushSlot(pendingEmission_, emissionTex_);
}

void DecalNode::releaseGL() {
    if (albedoTex_) { glDeleteTextures(1, &albedoTex_); albedoTex_ = 0; }
    if (emissionTex_) { glDeleteTextures(1, &emissionTex_); emissionTex_ = 0; }
}

} // namespace bro::scene
