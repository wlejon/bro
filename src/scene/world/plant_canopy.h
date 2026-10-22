#pragma once
// The plants' canopy (docs/scene-world-architecture.md §5, "the canopy
// layer"): what a plants node's rules grow at a point, as a function rather
// than a sum over placed plants, so the land can carry it where no plant is
// drawn or placed. Written as GLSL from the rules' fields (brokit's
// noise_glsl), the placement's own maths (world/plant_placement.h):
//
//   - a layer's candidates stand 1 / spacing^2 a square metre and grow by
//     their chance; a species' share is its weight over the weights' sum,
//     its size the scale field's with its variety (E[s^2] = scale^2 (1 +
//     variety^2 / 3));
//   - the crowns' projected area a square metre, tau, is summed over the
//     layers and species; the cover is 1 - e^-tau (a Poisson overlap);
//   - the top (a plant's height at its size over the ground) and the
//     leaves' albedo (the species' engine albedo) are their crowns'
//     area-weighted means.
//
// The module defines
//   vec2 plantsCanopy(vec2 p, out vec3 albedo)   x the cover, y the top (m)
// (shaders/ground/land_canopy.glsl is the module of no plants).

#include <string>

namespace bro::scene {

struct PlacementRules;

/// The canopy module of `rules` over `speciesCount` species records
/// (PlantsNode::kSpeciesFloats each); false, with why in `err`, when a field
/// has no GLSL (noise_glsl's unported nodes).
bool plantCanopyModule(const PlacementRules& rules, const float* species, int speciesCount, int speciesFloats,
                       std::string& out, std::string& err);

}  // namespace bro::scene
