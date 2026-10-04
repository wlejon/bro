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
    if (allocatedRes_ == resolution_)
        return true;
    releaseGL();

    const int res = resolution_;
    int mips = 1;
    for (int s = res; s > 8; s >>= 1) ++mips;

    allocatedRes_ = res;
    prefilterMips_ = mips;
    hasData_ = false;
    return true;
}

void ReflectionProbeNode::releaseGL() {
    captureCube_ = 0;
    prefilterCube_ = 0;
    allocatedRes_ = 0;
    prefilterMips_ = 0;
    hasData_ = false;
}

} // namespace bro::scene
