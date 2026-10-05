#pragma once

// Reflection probes.
//
// Capture: every visible probe that asks for one renders its six cube faces
// from its origin (90°, near 0.05, far at least 1000) into a mipmapped
// RGBA16F cube — the sky, then the frame's opaque meshes (static, skinned,
// instanced, custom), lit and shadowed by the frame's own lighting set with
// no probe, each face frustum-culled — and the shared GGX prefilter
// (SceneEnvironment::prefilter) turns it into the probe's specular cube,
// roughness down mips that end at an 8 px face. Translucent meshes, the
// overlay, terrain, decals, particles, splats and post are left out.
//
// Faces are rendered with the cube convention's ups and an unflipped
// projection (the y flip of Vulkan's clip space cancelled), so each face's
// rows land where cube sampling reads them; that mirrors the winding, so
// capture draws are MeshDraw::mirrored.
//
// Application: every probe holding a capture gets a lighting set of its own —
// the frame's lighting plus its box, intensity, blend margin and cube — and
// each mesh draw takes the set of the highest-priority probe whose box
// contains its bounds centre, ties to the smallest box (MeshDraw::lightingSet).
// Runs after the frame uniforms (the base lighting set) and before anything
// draws. Cubes are keyed by node id and dropped with their node.

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
    bool setup(SceneGpu&) override { return true; }
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) override;
    void cleanup(SceneGpu& gpu) override;

private:
    struct Probe {
        SceneVkImage capture;     // res², full mip chain
        SceneVkImage specular;    // res², GGX prefiltered down to 8 px
        std::array<VkImageView, 6> faces{};
        SceneVkImage depth;
        int resolution = 0;
        bool captured = false;
    };

    Probe* ensure(SceneGpu& gpu, const ReflectionProbeNode& node);
    void release(SceneGpu& gpu, Probe& probe);
    bool capture(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe);
    void renderFace(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe, int face);
    /// Give each probed draw its probe's lighting set.
    void assign(SceneFrame& frame);

    std::unordered_map<uint32_t, Probe> probes_;
};

}  // namespace bro::scene::vk
