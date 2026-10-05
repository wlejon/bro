#pragma once

// Reflection-probe capture. Every visible probe that asks for a capture
// renders its six cube faces (opaque meshes, unlit by scene lights, built-in
// shading) into a mipmapped RGBA16F cube, box-filtered down the chain; the
// highest-priority probe holding a capture becomes the frame's probe
// (SceneFrame::probe), which the lighting set samples. Cubes are keyed by
// node id and dropped when their node is destroyed.

#include "scene/vulkan/scene_pass.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace bro::scene {
class ReflectionProbeNode;
}

namespace bro::scene::vk {

class PassReflectionProbe final : public ScenePass {
public:
    const char* name() const override { return "reflection-probes"; }
    bool setup(SceneGpu& gpu) override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) override;
    void cleanup(SceneGpu& gpu) override;

private:
    struct Probe {
        SceneVkImage cube;
        std::array<VkImageView, 6> faces{};
        SceneVkImage depth;
        int resolution = 0;
        uint32_t mipLevels = 1;
    };

    Probe* ensure(SceneGpu& gpu, const ReflectionProbeNode& node);
    void release(SceneGpu& gpu, Probe& probe);
    void capture(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe);
    void renderFace(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe, int face);
    void buildMips(SceneFrame& frame, Probe& probe);

    std::unordered_map<uint32_t, Probe> probes_;
};

}  // namespace bro::scene::vk
