// coord.cpp — implementation of the pure grid coordinate + topology math.
//
// All deterministic integer math (line/ring hex paths use a brief float lerp +
// round, but the rounded result is exact). See coord.h for the contract.

#include "tile/coord.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bro::tile {

// -------------------------------------------------------------------------
// Canonical neighbour direction tables (index == direction).
// -------------------------------------------------------------------------

// Square Edge: 0:E 1:N 2:W 3:S   (+y is south/down).
static constexpr Cell kSquareEdge[4] = {
    { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 },
};

// Square Vertex: 0:E 1:NE 2:N 3:NW 4:W 5:SW 6:S 7:SE.
static constexpr Cell kSquareVertex[8] = {
    { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 },
    { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 },
};

// Hex axial neighbour deltas (canonical order 0..5), applied in axial space.
static constexpr Hex kHexAxial[6] = {
    { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 },
};

// -------------------------------------------------------------------------
// Offset <-> axial (pointy-top, odd-r).
// -------------------------------------------------------------------------

Hex toHex(Cell offset) {
    int q = offset.x - (offset.y - (offset.y & 1)) / 2;
    int r = offset.y;
    return Hex{ q, r };
}

Cell fromHex(Hex h) {
    int x = h.q + (h.r - (h.r & 1)) / 2;
    int y = h.r;
    return Cell{ x, y };
}

int hexDistance(Hex a, Hex b) {
    int ax = a.q, az = a.r, ay = -ax - az;
    int bx = b.q, bz = b.r, by = -bx - bz;
    return (std::abs(ax - bx) + std::abs(ay - by) + std::abs(az - bz)) / 2;
}

// -------------------------------------------------------------------------
// Neighbours
// -------------------------------------------------------------------------

Neighbors neighbors(Topology topo, Cell c, Conn conn) {
    Neighbors out;
    if (topo == Topology::Hex) {
        Hex h = toHex(c);
        for (int i = 0; i < 6; ++i) {
            Hex n{ h.q + kHexAxial[i].q, h.r + kHexAxial[i].r };
            out.items[out.count++] = fromHex(n);
        }
        return out;
    }

    if (conn == Conn::Vertex) {
        for (int i = 0; i < 8; ++i)
            out.items[out.count++] = Cell{ c.x + kSquareVertex[i].x, c.y + kSquareVertex[i].y };
    } else {
        for (int i = 0; i < 4; ++i)
            out.items[out.count++] = Cell{ c.x + kSquareEdge[i].x, c.y + kSquareEdge[i].y };
    }
    return out;
}

// -------------------------------------------------------------------------
// Distance
// -------------------------------------------------------------------------

int distance(Topology topo, Cell a, Cell b, Conn conn) {
    if (topo == Topology::Hex)
        return hexDistance(toHex(a), toHex(b));

    int dx = std::abs(a.x - b.x);
    int dy = std::abs(a.y - b.y);
    if (conn == Conn::Vertex)
        return dx > dy ? dx : dy;          // Chebyshev
    return dx + dy;                        // Manhattan
}

// -------------------------------------------------------------------------
// Ring (cells at exactly `radius`)
// -------------------------------------------------------------------------

std::vector<Cell> ring(Topology topo, Cell center, int radius, Conn conn) {
    std::vector<Cell> out;
    if (radius < 0)
        return out;
    if (radius == 0) {
        out.push_back(center);
        return out;
    }

    if (topo == Topology::Hex) {
        Hex h = toHex(center);
        // Start at center + axialDir(4) * radius, then walk 6 sides.
        Hex cur{ h.q + kHexAxial[4].q * radius, h.r + kHexAxial[4].r * radius };
        for (int side = 0; side < 6; ++side) {
            for (int step = 0; step < radius; ++step) {
                out.push_back(fromHex(cur));
                cur.q += kHexAxial[side].q;
                cur.r += kHexAxial[side].r;
            }
        }
        return out;
    }

    // Square. Scan the bounding box and keep cells whose metric == radius.
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int adx = std::abs(dx), ady = std::abs(dy);
            int metric = (conn == Conn::Vertex) ? (adx > ady ? adx : ady)
                                                : (adx + ady);
            if (metric == radius)
                out.push_back(Cell{ center.x + dx, center.y + dy });
        }
    }
    return out;
}

// -------------------------------------------------------------------------
// Range (filled disk, inclusive of center)
// -------------------------------------------------------------------------

std::vector<Cell> range(Topology topo, Cell center, int radius, Conn conn) {
    std::vector<Cell> out;
    if (radius < 0)
        return out;

    if (topo == Topology::Hex) {
        Hex h = toHex(center);
        for (int dq = -radius; dq <= radius; ++dq) {
            int lo = std::max(-radius, -dq - radius);
            int hi = std::min(radius, -dq + radius);
            for (int dr = lo; dr <= hi; ++dr)
                out.push_back(fromHex(Hex{ h.q + dq, h.r + dr }));
        }
        return out;
    }

    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int adx = std::abs(dx), ady = std::abs(dy);
            int metric = (conn == Conn::Vertex) ? (adx > ady ? adx : ady)
                                                : (adx + ady);
            if (metric <= radius)
                out.push_back(Cell{ center.x + dx, center.y + dy });
        }
    }
    return out;
}

// -------------------------------------------------------------------------
// Line
// -------------------------------------------------------------------------

namespace {

// Round a fractional cube coordinate to the nearest valid hex (x+y+z == 0).
Hex cubeRound(double x, double y, double z) {
    double rx = std::round(x);
    double ry = std::round(y);
    double rz = std::round(z);
    double dx = std::abs(rx - x);
    double dy = std::abs(ry - y);
    double dz = std::abs(rz - z);
    if (dx > dy && dx > dz)
        rx = -ry - rz;
    else if (dy > dz)
        ry = -rx - rz;
    else
        rz = -rx - ry;
    return Hex{ static_cast<int>(rx), static_cast<int>(rz) }; // axial q=x, r=z
}

} // namespace

std::vector<Cell> line(Topology topo, Cell a, Cell b) {
    std::vector<Cell> out;

    if (topo == Topology::Hex) {
        Hex ha = toHex(a), hb = toHex(b);
        int N = hexDistance(ha, hb);
        if (N == 0) {
            out.push_back(a);
            return out;
        }
        double ax = ha.q, az = ha.r, ay = -ax - az;
        double bx = hb.q, bz = hb.r, by = -bx - bz;
        for (int i = 0; i <= N; ++i) {
            double t = static_cast<double>(i) / static_cast<double>(N);
            double x = ax + (bx - ax) * t;
            double y = ay + (by - ay) * t;
            double z = az + (bz - az) * t;
            out.push_back(fromHex(cubeRound(x, y, z)));
        }
        return out;
    }

    // Square 4-connected supercover: every step changes exactly one axis by 1.
    int x = a.x, y = a.y;
    int nx = std::abs(b.x - a.x);
    int ny = std::abs(b.y - a.y);
    int sx = (b.x > a.x) ? 1 : -1;
    int sy = (b.y > a.y) ? 1 : -1;

    out.push_back(Cell{ x, y });
    int ix = 0, iy = 0;
    while (ix < nx || iy < ny) {
        bool horizontal;
        if (iy >= ny) {
            horizontal = true;
        } else if (ix >= nx) {
            horizontal = false;
        } else {
            // Compare crossing parameters; tie -> horizontal first (deterministic,
            // keeps the walk 4-connected with no diagonal-only step).
            long long decision = static_cast<long long>(1 + 2 * ix) * ny -
                                 static_cast<long long>(1 + 2 * iy) * nx;
            horizontal = (decision <= 0);
        }
        if (horizontal) {
            x += sx;
            ++ix;
        } else {
            y += sy;
            ++iy;
        }
        out.push_back(Cell{ x, y });
    }
    return out;
}

} // namespace bro::tile
