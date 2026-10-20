#include "scene/vulkan/pass_far_cards.h"

#include "scene/vulkan/scene_far_cards.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"

namespace bro::scene::vk {

bool PassFarCardCapture::active(const SceneFrame&) const { return cards_.capturing(); }

void PassFarCardCapture::declare(const SceneFrame& frame, PassIO& io) const {
    // The captures bind a lighting set, as the frame's opaque draws do: its
    // shadow atlas and pyramids must be readable (their far depth is the
    // cards' own).
    io.sample(frame.gpu.targets.shadowAtlas);
    io.sample(frame.gpu.targets.farDepth);
    io.sample(frame.gpu.targets.hiZ);
    io.sample(frame.gpu.targets.hiZPrev);
}

void PassFarCardCapture::record(SceneFrame& frame) { cards_.capture(frame); }

bool PassFarCardDraw::active(const SceneFrame&) const { return cards_.drawing(); }

void PassFarCardDraw::declare(const SceneFrame& frame, PassIO& io) const { io.hdr({frame.ssao}); }

void PassFarCardDraw::record(SceneFrame& frame) { cards_.draw(frame); }

}  // namespace bro::scene::vk
