#include "scene/reflection_probe_node.h"

#include "util/log.h"

namespace bro::scene {

ReflectionProbeNode::ReflectionProbeNode(const std::string& name)
    : SceneNode(name) {}

void ReflectionProbeNode::setResolution(int r) {
    // Clamp to a power of two in [16, 1024] (round down to the nearest pow2
    // so any in-range request is honored predictably).
    if (r < 16) r = 16;
    if (r > 1024) r = 1024;
    int p = 16;
    while (p * 2 <= r) p *= 2;
    resolution_ = p;
    // Takes effect on the next capture: the probe pass reallocates when its
    // cubemap no longer matches.
}

} // namespace bro::scene
