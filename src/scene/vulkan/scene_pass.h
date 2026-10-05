#pragma once

// The unit the scene renderer is built from. A render is an ordered list of
// ScenePasses run by SceneFrameGraph (scene_frame_graph.h); each pass says
// what it touches and records its commands, and the graph does the layout
// transitions and opens the shared HDR raster scope around the passes that
// draw into it.
//
// Adding a pass is local: write a ScenePass, construct it in SceneVkBridge,
// and insert it into the pass list where it belongs. A pass that draws into
// the HDR colour/depth target asks for the scope in declare() and records
// draws only; a pass with targets of its own (shadow maps, a probe's cube,
// a post-process chain) declares the shared images it reads and writes, and
// begins and ends its own rendering inside record().

#include "scene/vulkan/scene_vk_allocator.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

/// How a pass touches a shared frame image.
enum class ImageAccess : uint8_t {
    Sampled,          // read in a shader
    ColorTarget,      // rendered into as a colour attachment (own rendering)
    DepthTarget,      // rendered into as a depth attachment (own rendering)
    TransferSrc,
    TransferDst,
};

/// The HDR raster scope: the frame's colour + depth target (MSAA when the
/// renderer asks for it, resolved at the end of every scope), plus the
/// indirect-light attachment while SSAO needs it. The frame's first scope
/// clears colour and depth; every later one loads them.
struct HdrScope {
    bool indirect = false;   // carries the indirect-light attachment
};

class PassIO {
public:
    void sample(SceneVkImage& image) { add(image, ImageAccess::Sampled); }
    void colorTarget(SceneVkImage& image) { add(image, ImageAccess::ColorTarget); }
    void depthTarget(SceneVkImage& image) { add(image, ImageAccess::DepthTarget); }
    void transferSrc(SceneVkImage& image) { add(image, ImageAccess::TransferSrc); }
    void transferDst(SceneVkImage& image) { add(image, ImageAccess::TransferDst); }

    /// Draw into the HDR scope. Consecutive passes that ask for the same
    /// attachments share one scope.
    void hdr(HdrScope scope = {}) { hdr_ = true; scope_ = scope; }

    struct Use { SceneVkImage* image; ImageAccess access; };
    const std::vector<Use>& uses() const { return uses_; }
    bool drawsHdr() const { return hdr_; }
    const HdrScope& scope() const { return scope_; }

private:
    void add(SceneVkImage& image, ImageAccess access) {
        if (image.isValid()) uses_.push_back({&image, access});
    }

    std::vector<Use> uses_;
    bool hdr_ = false;
    HdrScope scope_{};
};

class ScenePass {
public:
    virtual ~ScenePass() = default;

    virtual const char* name() const = 0;

    /// Create pipelines, layouts and fixed resources. False fails the renderer.
    virtual bool setup(SceneGpu& gpu) = 0;
    /// The frame targets changed size (always called once before the first frame).
    virtual void resize(SceneGpu& /*gpu*/, uint32_t /*width*/, uint32_t /*height*/) {}

    /// Whether the pass runs this frame (settings, content).
    virtual bool active(const SceneFrame& /*frame*/) const { return true; }
    /// The shared images the pass reads and writes, and whether it draws
    /// into the HDR scope. Called right before record(); the graph moves each
    /// image into the layout its access needs (closing an open HDR scope
    /// first when one has to change) and opens or keeps the scope.
    virtual void declare(const SceneFrame& /*frame*/, PassIO& /*io*/) const {}
    virtual void record(SceneFrame& frame) = 0;

    /// Nodes were destroyed: drop what the pass holds for them (deferred
    /// through the frame core, never waited for).
    virtual void releaseNodes(SceneGpu& /*gpu*/, std::span<const uint32_t> /*ids*/) {}
    virtual void cleanup(SceneGpu& gpu) = 0;
};

}  // namespace bro::scene::vk
