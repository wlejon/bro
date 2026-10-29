#pragma once
// The plants' canopy: what a plants node's rules grow at a point, as a function rather
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
//     area-weighted means;
//   - the crowns' flanks a square metre, as tau is summed: each crown seen
//     from the side (its depth, the height over its base, by its width; a
//     round crown's ellipse, a cone's triangle). Seen along a line e over
//     the level a crown covers its area plus its flank times cot e, so the
//     cover the eye sees is 1 - e^-(tau + flanks cot e) (mesh.frag's
//     landCanopyAt): a wood is whole from a low line where its crowns
//     stand apart from overhead;
//   - the crowns' base (a plant's trunk under its crown over the ground),
//     their area-weighted mean as the top is: how far a line climbs
//     before it meets a crown (mesh.frag's landCanopyGround).
//
// The module defines
//   vec4 plantsCanopy(vec2 p, out vec3 albedo)   x the cover, y the top (m),
//                                                z the flanks, w the base (m)
//   vec4 plantsCanopy(vec3 p, out vec3 albedo)   the same on a round body, p
//                                                the datum's point (its
//                                                fields' 3D trees, split
//                                                about the fill's origin)
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
