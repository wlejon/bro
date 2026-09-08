#pragma once

// A shade map is a per-cell scalar a TileWorld hands to every node it draws:
// ground chunks, overlay decals and object kinds. The fragment shader looks
// the cell up from the fragment's world XZ and multiplies the LIT colour by
// it, after lighting, ambient and scene fog, so a 0 is black whatever the
// lights do — the difference from a tint, which multiplies the albedo and
// still leaves specular and ambient on the surface.
//
// The binding is resolved per draw through a provider so the texture can be
// uploaded lazily on the GL thread and the owner (the TileWorld) keeps one
// texture for every node that samples it. A provider returning false means
// "no shade this draw" and costs the draw one uniform.

#include <bromath/vec.h>
#include <glad/gl.h>

#include <functional>

namespace bro::scene {

struct ShadeMapBinding {
    GLuint tex = 0;                 // R8 texture, one texel per cell
    bromath::Vec3 origin;           // world position of cell (0, 0)'s grid origin
    float cellSize = 1.0f;          // world units per cell (hex: circumradius)
    bool hex = false;               // pointy-top odd-r hex grid, else square
    int width = 0;
    int height = 0;
};

using ShadeMapProvider = std::function<bool(ShadeMapBinding&)>;

} // namespace bro::scene
