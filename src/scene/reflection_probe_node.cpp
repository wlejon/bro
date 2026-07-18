#include "scene/reflection_probe_node.h"

#include "util/log.h"

namespace bro::scene {

ReflectionProbeNode::ReflectionProbeNode(const std::string& name)
    : SceneNode(name) {}

ReflectionProbeNode::~ReflectionProbeNode() {
    releaseGL();
}

void ReflectionProbeNode::setResolution(int r) {
    // Clamp to a power of two in [16, 1024] (round down to the nearest pow2
    // so any in-range request is honored predictably).
    if (r < 16) r = 16;
    if (r > 1024) r = 1024;
    int p = 16;
    while (p * 2 <= r) p *= 2;
    resolution_ = p;
    // Takes effect on the next capture: ensureTextures() reallocates when
    // allocatedRes_ no longer matches.
}

bool ReflectionProbeNode::ensureTextures() {
    if (captureCube_ && prefilterCube_ && allocatedRes_ == resolution_)
        return true;
    releaseGL();

    const int res = resolution_;
    // Prefilter mip count: chain down to an 8px face (matches the global
    // env prefilter's 256² x 6 mips density). res 128 -> 5 mips.
    int mips = 1;
    for (int s = res; s > 8; s >>= 1) ++mips;

    glGenTextures(1, &captureCube_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, captureCube_);
    for (int f = 0; f < 6; ++f) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                     res, res, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Allocate the full mip chain upfront: the GGX prefilter samples the
    // capture with Krivanek mip-bias LODs, and glGenerateMipmap after each
    // capture refills these levels.
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    glGenTextures(1, &prefilterCube_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, prefilterCube_);
    for (int f = 0; f < 6; ++f) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                     res, res, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Reserve mip storage so per-mip FBO attachment works in the prefilter.
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    if (!captureCube_ || !prefilterCube_) {
        LOG_ERROR("ReflectionProbeNode: cubemap allocation failed (res %d)", res);
        releaseGL();
        return false;
    }

    allocatedRes_ = res;
    prefilterMips_ = mips;
    // Freshly reallocated textures hold no capture.
    hasData_ = false;
    return true;
}

void ReflectionProbeNode::releaseGL() {
    if (captureCube_) { glDeleteTextures(1, &captureCube_); captureCube_ = 0; }
    if (prefilterCube_) { glDeleteTextures(1, &prefilterCube_); prefilterCube_ = 0; }
    allocatedRes_ = 0;
    prefilterMips_ = 0;
    hasData_ = false;
}

} // namespace bro::scene
