#pragma once

// A shade map is a per-cell scalar a TileWorld hands to every node it draws:
// ground chunks, overlay decals and object kinds. The fragment shader looks
// the cell up from the fragment's world XZ and multiplies the LIT colour by
// it, after lighting, ambient and scene fog, so a 0 is black whatever the
// lights do — the difference from a tint, which multiplies the albedo and
// still leaves specular and ambient on the surface.
//
// The binding is resolved per frame through a provider, so the owner (the
// TileWorld) keeps one CPU map for every node that samples it and the
// renderer keeps one GPU copy, re-uploaded when `generation` moves. A
// provider returning false means "no shade".

#include <bromath/vec.h>
#include <cstdint>

#include <functional>

namespace bro::scene {

struct ShadeMapBinding {
    const uint8_t* pixels = nullptr;// R8, one byte per cell, row-major
    uint64_t generation = 0;        // moves whenever any cell changes
    bromath::Vec3 origin;           // world position of cell (0, 0)'s grid origin
    float cellSize = 1.0f;          // world units per cell (hex: circumradius)
    bool hex = false;               // pointy-top odd-r hex grid, else square
    int width = 0;
    int height = 0;
};

using ShadeMapProvider = std::function<bool(ShadeMapBinding&)>;

} // namespace bro::scene
