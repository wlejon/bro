#pragma once

#include "scene/tile_world.h"
#include <array>
#include <cmath>

namespace bro::scene {

struct HexVec2 { float x, z; };
constexpr float kHexDeg2Rad = 3.14159265358979323846f / 180.0f;

inline const std::array<HexVec2, 6>& hexCorners() {
    static const std::array<HexVec2, 6> c = [] {
        std::array<HexVec2, 6> arr{};
        for (int i = 0; i < 6; ++i) {
            float a = (30.0f - 60.0f * static_cast<float>(i)) * kHexDeg2Rad;
            arr[i] = {std::cos(a), std::sin(a)};
        }
        return arr;
    }();
    return c;
}

inline const std::array<HexVec2, 6>& hexDirs() {
    static const std::array<HexVec2, 6> d = [] {
        std::array<HexVec2, 6> arr{};
        for (int i = 0; i < 6; ++i) {
            float a = (-60.0f * static_cast<float>(i)) * kHexDeg2Rad;
            arr[i] = {std::cos(a), std::sin(a)};
        }
        return arr;
    }();
    return d;
}

} // namespace bro::scene
