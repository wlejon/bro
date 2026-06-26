#pragma once

// coord.h — pure grid coordinate + topology math for the tile core.
//
// NO rendering, NO scene, NO allocation in the hot paths that can avoid it.
// Everything here is deterministic integer math, unit-testable in isolation,
// and shared by every higher layer (grid storage, regions, autotiling,
// pathfinding). Two topologies are supported as first-class citizens:
//
//   Square  — cells indexed by integer (x, y). Connectivity is selectable:
//             Edge (4-neighbour, von Neumann) or Vertex (8-neighbour, Moore).
//   Hex     — pointy-top hexagons stored in "odd-r" offset coordinates
//             (odd rows shoved +half right). Always 6-neighbour; the Conn
//             argument is ignored. Internally hex math runs in axial (q, r)
//             space and converts to/from offset at the boundary.
//
// All storage everywhere uses offset (x, y) == (col, row); Hex axial is an
// implementation detail exposed only via toHex/fromHex for callers that want
// to do hex arithmetic directly.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace bro::tile {

// -------------------------------------------------------------------------
// Topology + connectivity
// -------------------------------------------------------------------------

enum class Topology : uint8_t {
    Square = 0,
    Hex    = 1,
};

// Square connectivity. Hex ignores this (always 6 edge-neighbours).
//   Edge   — 4 orthogonal neighbours; distance is Manhattan; range is a diamond.
//   Vertex — 8 neighbours incl. diagonals; distance is Chebyshev; range is a box.
enum class Conn : uint8_t {
    Edge   = 0,
    Vertex = 1,
};

// -------------------------------------------------------------------------
// Coordinates
// -------------------------------------------------------------------------

// Offset coordinate (col, row). The universal storage coordinate for both
// topologies.
struct Cell {
    int x = 0;
    int y = 0;
    constexpr bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
    constexpr bool operator!=(const Cell& o) const { return !(*this == o); }
};

// Axial hex coordinate. Cube coords are (q, r, -q-r). Only meaningful when the
// grid topology is Hex.
struct Hex {
    int q = 0;
    int r = 0;
    constexpr bool operator==(const Hex& o) const { return q == o.q && r == o.r; }
    constexpr bool operator!=(const Hex& o) const { return !(*this == o); }
};

// Hash for use in unordered_set/map keyed by Cell (region/pathfind internals).
struct CellHash {
    size_t operator()(const Cell& c) const {
        // 32-bit pack then mix; coords are well within int32 in practice.
        uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32) |
                      static_cast<uint32_t>(c.y);
        k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
        return static_cast<size_t>(k);
    }
};

// -------------------------------------------------------------------------
// Neighbour result — fixed capacity, allocation-free.
// -------------------------------------------------------------------------
//
// Square Edge -> 4, Square Vertex -> 8, Hex -> 6. Direction order is canonical
// and STABLE (callers rely on index == direction):
//
//   Square Edge:   0:E  1:N  2:W  3:S
//   Square Vertex: 0:E  1:NE 2:N  3:NW 4:W  5:SW 6:S  7:SE
//   Hex (pointy):  0:E  1:NE 2:NW 3:W  4:SW 5:SE   (axial dirs below)
//
// (+y is "south"/down in offset space, matching row-major storage.)
struct Neighbors {
    std::array<Cell, 8> items{};
    int count = 0;

    const Cell* begin() const { return items.data(); }
    const Cell* end() const { return items.data() + count; }
    Cell operator[](int i) const { return items[i]; }
};

// -------------------------------------------------------------------------
// Topology operations
// -------------------------------------------------------------------------

// Neighbours of `c` in canonical order (see Neighbors doc above). Does NOT
// bounds-check — callers filter against a grid's bounds.
Neighbors neighbors(Topology topo, Cell c, Conn conn = Conn::Edge);

// Grid distance between two cells.
//   Square Edge   -> Manhattan |dx|+|dy|
//   Square Vertex -> Chebyshev max(|dx|,|dy|)
//   Hex           -> hex distance (cube metric), conn ignored
int distance(Topology topo, Cell a, Cell b, Conn conn = Conn::Edge);

// All cells at EXACTLY `radius` from center (the hollow ring). radius==0 -> {center}.
//   Square Edge   -> diamond ring (Manhattan == radius)
//   Square Vertex -> box ring (Chebyshev == radius)
//   Hex           -> hex ring
std::vector<Cell> ring(Topology topo, Cell center, int radius, Conn conn = Conn::Edge);

// All cells within `radius` (filled disk, inclusive of center). Order is
// unspecified but deterministic for a given (topo, center, radius, conn).
std::vector<Cell> range(Topology topo, Cell center, int radius, Conn conn = Conn::Edge);

// A connected grid line from a to b inclusive.
//   Square -> Bresenham supercover (every cell the segment passes through,
//             4-connected so there are no diagonal gaps)
//   Hex    -> cube-lerp hex line
std::vector<Cell> line(Topology topo, Cell a, Cell b);

// -------------------------------------------------------------------------
// Hex offset <-> axial conversions (pointy-top, odd-r).
// -------------------------------------------------------------------------
Hex  toHex(Cell offset);
Cell fromHex(Hex h);

// Hex distance in axial/cube space (exposed for callers doing hex math
// directly; `distance(Topology::Hex, ...)` routes here).
int  hexDistance(Hex a, Hex b);

} // namespace bro::tile
