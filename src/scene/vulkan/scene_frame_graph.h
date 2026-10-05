#pragma once

// Runs a frame's ScenePasses in order. For each active pass it asks what the
// pass touches (ScenePass::declare), moves those images into the layouts the
// accesses need, opens, keeps or closes the shared HDR raster scope, and
// records the pass. The passes themselves never transition a shared image.
//
// The HDR scope renders into SceneTargets' HDR colour and depth (their MSAA
// twins when multisampled, resolved into the single-sample images at the end
// of every scope) and, when a pass asks for it, the indirect-light
// attachment. The first scope of a frame clears colour to transparent black
// and depth to the far plane; later scopes load what the earlier ones left.
// Closing a scope makes everything it wrote — resolves included — visible to
// whatever comes next.

#include "scene/vulkan/scene_pass.h"

#include <memory>
#include <span>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

class SceneFrameGraph {
public:
    /// Append a pass; passes run in the order they were added.
    ScenePass& add(std::unique_ptr<ScenePass> pass);

    bool setup(SceneGpu& gpu);
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height);
    void run(SceneFrame& frame);
    void releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids);
    void cleanup(SceneGpu& gpu);

    /// Move `image` (every mip and layer) into `layout`, recording the barrier
    /// when it is not there already.
    static void transition(VkCommandBuffer cmd, SceneVkImage& image, VkImageLayout layout);

private:
    void openScope(SceneFrame& frame, const HdrScope& scope);
    void closeScope(SceneFrame& frame);

    std::vector<std::unique_ptr<ScenePass>> passes_;
    bool scopeOpen_ = false;
    bool scopeIndirect_ = false;
    bool firstScope_ = true;
    bool indirectCleared_ = false;
};

}  // namespace bro::scene::vk
