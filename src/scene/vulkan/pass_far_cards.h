#pragma once
// The far cards' two passes (scene_far_cards.h): the captures, after the
// shadows and the frame's lighting and before anything draws into the HDR
// target, and the cards, in the HDR scope after the opaque batches (before
// the far depth the pyramid is built from, so the cards occlude).

#include "scene/vulkan/scene_pass.h"

namespace bro::scene::vk {

class FarCards;

class PassFarCardCapture final : public ScenePass {
public:
    explicit PassFarCardCapture(FarCards& cards) : cards_(cards) {}
    const char* name() const override { return "far-cards:capture"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    FarCards& cards_;
};

class PassFarCardDraw final : public ScenePass {
public:
    explicit PassFarCardDraw(FarCards& cards) : cards_(cards) {}
    const char* name() const override { return "far-cards"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    FarCards& cards_;
};

}  // namespace bro::scene::vk
