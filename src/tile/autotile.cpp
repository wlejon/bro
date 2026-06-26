// autotile.cpp — neighbour bitmask -> tile variant index.

#include "tile/autotile.h"

#include <array>
#include <cstdint>

namespace bro::tile {

// ---- direction masks -----------------------------------------------------

uint8_t edgeMask(const TileGrid& g, Cell c, const FamilyFn& fam) {
    if (g.topology() == Topology::Hex) {
        // 6 bits in canonical hex neighbour order (index 0..5 from coord.h).
        Neighbors nb = neighbors(Topology::Hex, c);
        uint8_t m = 0;
        for (int i = 0; i < nb.count; ++i)
            if (fam(g, nb[i])) m |= static_cast<uint8_t>(1u << i);
        return m;
    }
    // Square: E,N,W,S = bits 0,1,2,3.
    static const Cell d[4] = { {1, 0}, {0, -1}, {-1, 0}, {0, 1} };
    uint8_t m = 0;
    for (int i = 0; i < 4; ++i)
        if (fam(g, Cell{ c.x + d[i].x, c.y + d[i].y })) m |= static_cast<uint8_t>(1u << i);
    return m;
}

uint8_t blobMask(const TileGrid& g, Cell c, const FamilyFn& fam) {
    if (g.topology() == Topology::Hex) return edgeMask(g, c, fam);
    // Square: Vertex order E,NE,N,NW,W,SW,S,SE = bits 0..7.
    static const Cell d[8] = {
        { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 },
        { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 }
    };
    uint8_t m = 0;
    for (int i = 0; i < 8; ++i)
        if (fam(g, Cell{ c.x + d[i].x, c.y + d[i].y })) m |= static_cast<uint8_t>(1u << i);
    return m;
}

// ---- 47-blob reduction ---------------------------------------------------

// A corner bit only counts when BOTH its adjacent edge bits are set; otherwise
// the corner bit is cleared. Edge bits (E=0,N=2,W=4,S=6) always pass through.
//   NE=bit1 adj E(0),N(2)   NW=bit3 adj N(2),W(4)
//   SW=bit5 adj W(4),S(6)   SE=bit7 adj S(6),E(0)
static uint8_t normalizeBlob(uint8_t m) {
    uint8_t out = m & 0x55; // keep all edge bits (0,2,4,6); start corners cleared
    auto cornerOk = [&](int corner, int e0, int e2) {
        return (m & (1u << corner)) && (m & (1u << e0)) && (m & (1u << e2));
    };
    if (cornerOk(1, 0, 2)) out |= 1u << 1; // NE
    if (cornerOk(3, 2, 4)) out |= 1u << 3; // NW
    if (cornerOk(5, 4, 6)) out |= 1u << 5; // SW
    if (cornerOk(7, 6, 0)) out |= 1u << 7; // SE
    return out;
}

// Build the mask(0..255) -> dense index(0..46) table once. Distinct normalized
// forms are numbered in increasing normalized-mask order, giving a deterministic
// scheme: two masks with the same normalized form share an index.
static const std::array<int, 256>& blobTable() {
    static const std::array<int, 256> table = [] {
        std::array<int, 256> norm{};
        std::array<int, 256> seen{};
        for (int i = 0; i < 256; ++i) seen[i] = -1;
        for (int m = 0; m < 256; ++m) norm[m] = normalizeBlob(static_cast<uint8_t>(m));
        // Assign dense indices in increasing normalized-mask order.
        int next = 0;
        for (int nm = 0; nm < 256; ++nm)
            if (seen[nm] == -1) {
                // Only assign to normalized forms that actually occur.
                bool occurs = false;
                for (int m = 0; m < 256 && !occurs; ++m)
                    if (norm[m] == nm) occurs = true;
                if (occurs) seen[nm] = next++;
            }
        std::array<int, 256> out{};
        for (int m = 0; m < 256; ++m) out[m] = seen[norm[m]];
        return out;
    }();
    return table;
}

int blob47(uint8_t blobMask8) {
    return blobTable()[blobMask8];
}

uint8_t wangCorners(const TileGrid& g, Cell c, const FamilyFn& fam) {
    // NE,SE,SW,NW = bits 0,1,2,3.
    static const Cell d[4] = { { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
    uint8_t m = 0;
    for (int i = 0; i < 4; ++i)
        if (fam(g, Cell{ c.x + d[i].x, c.y + d[i].y })) m |= static_cast<uint8_t>(1u << i);
    return m;
}

// ---- family predicate factories ------------------------------------------

FamilyFn familyTile(int layer, uint16_t id) {
    return [layer, id](const TileGrid& g, Cell c) {
        return g.inBounds(c) && g.tile(layer, c) == id;
    };
}

FamilyFn familyNonEmpty(int layer) {
    return [layer](const TileGrid& g, Cell c) {
        return g.inBounds(c) && g.tile(layer, c) != 0;
    };
}

} // namespace bro::tile
