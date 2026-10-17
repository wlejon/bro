#pragma once

// Forest impostors: what a specimen's leaves tier looks like from many
// directions, so a far tree can be one camera-facing card that is the near
// tree's own picture rather than a stand-in for it.
//
// Each specimen is baked on the CPU from exactly what the leaves tier draws
// at full detail (every leaf where the clump-mode scatter puts it, the leaf
// mesh's own triangles and normals, the bark), orthographically from N x N
// directions over the upper hemisphere, HEMI-OCTAHEDRAL: cell (i, j) looks
// from impostorViewDir(i, j, N). Two RGBA8 images, one block of N x N cells
// a specimen:
//   colour  rgb the albedo the leaves tier lights (the leaf colour times
//           vegetationTint without its per-tree part, the paler underside
//           where the view sees one; the bark colour on wood), sRGB; a the
//           coverage, supersampled 2 x 2
//   data    rg the normal in the cell's basis (x right, y up, facing the
//           view, so its z is implied), b the depth toward the view over the
//           card's half size, a the leafiness (1 leaf, 0 bark)
// Empty texels carry their neighbours' values (alpha 0), so mipmaps and
// bilinear reads never pull black in.
//
// The renderer (scene_forest_impostor.glsl) lights the picture at run time
// with the leaves' own lighting: the crown's bulk normal and ambient
// occlusion from the reconstructed position, translucency, the shadow map.
// Light and shade therefore follow the sun and stay put while the tree
// moves; only the pose moves the card.

#include <bromath/vec.h>

#include <cstdint>
#include <vector>

namespace bro::scene {

class ForestNode;

struct ForestImpostorAtlas {
    int views = 0;      // N: cells a side of a specimen's block
    int texels = 0;     // a cell's side
    int blocksX = 0;    // blocks a row of the atlas
    int width = 0, height = 0;
    std::vector<uint8_t> color, data;   // width * height * 4 each
    /// 4 floats a specimen: the card's centre (specimen space) and half size.
    std::vector<float> spheres;
    /// 4 floats a specimen: the upright cylinder round the same centre that
    /// holds it (radius, half height; then two zeros), which fits a card
    /// to a direction tighter than the sphere.
    std::vector<float> shapes;
    bool empty() const { return width <= 0 || height <= 0; }
};

/// The view direction (toward the viewer, unit, y >= 0) of cell (i, j) of an
/// n x n hemi-octahedral grid; scene_forest_impostor.glsl's forestImpostorDir.
bromath::Vec3 forestImpostorViewDir(int i, int j, int n);
/// A view's image axes: `right` and `up` across it for a view from `f`.
void forestImpostorBasis(const bromath::Vec3& f, bromath::Vec3& right, bromath::Vec3& up);

/// Bake `node`'s specimens with its leaf mesh, leaf colour and bark colour.
/// `views` and `texels` as ForestNode::Lod; texels shrink so the atlas fits
/// `maxSide`. Uses every hardware thread.
ForestImpostorAtlas bakeForestImpostors(const ForestNode& node, int views, int texels, int maxSide = 16384);

}  // namespace bro::scene
