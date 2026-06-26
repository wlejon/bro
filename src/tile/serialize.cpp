#include "tile/serialize.h"

#include <cstddef>
#include <string>

namespace bro::tile {

namespace {

// ---- little-endian append helpers -----------------------------------------
void putU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void putI16(std::vector<uint8_t>& out, int16_t v) {
    putU16(out, static_cast<uint16_t>(v));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void putI32(std::vector<uint8_t>& out, int32_t v) {
    putU32(out, static_cast<uint32_t>(v));
}

// ---- little-endian read helpers (bounds-checked) --------------------------
// Each advances `off` and sets `ok=false` (leaving prior failures sticky) if
// there are not enough bytes remaining.
uint8_t getU8(const std::vector<uint8_t>& b, size_t& off, bool& ok) {
    if (!ok || off + 1 > b.size()) { ok = false; return 0; }
    return b[off++];
}

uint16_t getU16(const std::vector<uint8_t>& b, size_t& off, bool& ok) {
    if (!ok || off + 2 > b.size()) { ok = false; return 0; }
    uint16_t v = static_cast<uint16_t>(b[off]) |
                 (static_cast<uint16_t>(b[off + 1]) << 8);
    off += 2;
    return v;
}

int16_t getI16(const std::vector<uint8_t>& b, size_t& off, bool& ok) {
    return static_cast<int16_t>(getU16(b, off, ok));
}

uint32_t getU32(const std::vector<uint8_t>& b, size_t& off, bool& ok) {
    if (!ok || off + 4 > b.size()) { ok = false; return 0; }
    uint32_t v = static_cast<uint32_t>(b[off]) |
                 (static_cast<uint32_t>(b[off + 1]) << 8) |
                 (static_cast<uint32_t>(b[off + 2]) << 16) |
                 (static_cast<uint32_t>(b[off + 3]) << 24);
    off += 4;
    return v;
}

int32_t getI32(const std::vector<uint8_t>& b, size_t& off, bool& ok) {
    return static_cast<int32_t>(getU32(b, off, ok));
}

} // namespace

std::vector<uint8_t> serialize(const TileGrid& g) {
    std::vector<uint8_t> out;

    // Header
    putU8(out, 'B');
    putU8(out, 'T');
    putU8(out, 'I');
    putU8(out, 'L');
    putU8(out, 1); // version
    putU8(out, static_cast<uint8_t>(g.topology()));
    putU8(out, static_cast<uint8_t>(g.layerCount()));
    putU8(out, 0); // reserved
    putI32(out, g.width());
    putI32(out, g.height());

    // Layer names
    const std::vector<std::string>& names = g.layerNames();
    for (const std::string& name : names) {
        putU16(out, static_cast<uint16_t>(name.size()));
        for (char ch : name)
            putU8(out, static_cast<uint8_t>(ch));
    }

    // Layer planes (layer-major then row-major)
    const int layers = g.layerCount();
    for (int layer = 0; layer < layers; ++layer) {
        const std::vector<uint16_t>& plane = g.layerData(layer);
        for (uint16_t id : plane)
            putU16(out, id);
    }

    // Elevation plane
    const std::vector<int16_t>& elev = g.elevationData();
    for (int16_t e : elev)
        putI16(out, e);

    // Flag plane
    const std::vector<uint32_t>& flags = g.flagData();
    for (uint32_t f : flags)
        putU32(out, f);

    return out;
}

std::optional<TileGrid> deserialize(const std::vector<uint8_t>& bytes) {
    size_t off = 0;
    bool ok = true;

    // Magic
    uint8_t m0 = getU8(bytes, off, ok);
    uint8_t m1 = getU8(bytes, off, ok);
    uint8_t m2 = getU8(bytes, off, ok);
    uint8_t m3 = getU8(bytes, off, ok);
    if (!ok || m0 != 'B' || m1 != 'T' || m2 != 'I' || m3 != 'L')
        return std::nullopt;

    uint8_t version = getU8(bytes, off, ok);
    if (!ok || version != 1)
        return std::nullopt;

    uint8_t topoByte = getU8(bytes, off, ok);
    if (!ok || topoByte > 1)
        return std::nullopt;

    uint8_t layerCount = getU8(bytes, off, ok);
    if (!ok || layerCount < 1)
        return std::nullopt;

    getU8(bytes, off, ok); // reserved (ignored)

    int32_t width = getI32(bytes, off, ok);
    int32_t height = getI32(bytes, off, ok);
    if (!ok || width < 1 || height < 1)
        return std::nullopt;

    // Layer names
    std::vector<std::string> names;
    names.reserve(layerCount);
    for (int i = 0; i < layerCount; ++i) {
        uint16_t nameLen = getU16(bytes, off, ok);
        if (!ok)
            return std::nullopt;
        std::string name;
        name.reserve(nameLen);
        for (uint16_t j = 0; j < nameLen; ++j) {
            uint8_t ch = getU8(bytes, off, ok);
            if (!ok)
                return std::nullopt;
            name.push_back(static_cast<char>(ch));
        }
        names.push_back(std::move(name));
    }

    const size_t cellCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    const Topology topo = static_cast<Topology>(topoByte);

    TileGrid grid(width, height, topo, names);

    // Layer planes (layer-major then row-major)
    for (int layer = 0; layer < layerCount; ++layer) {
        for (size_t idx = 0; idx < cellCount; ++idx) {
            uint16_t id = getU16(bytes, off, ok);
            if (!ok)
                return std::nullopt;
            grid.setTile(layer, grid.cellOf(idx), id);
        }
    }

    // Elevation plane
    for (size_t idx = 0; idx < cellCount; ++idx) {
        int16_t e = getI16(bytes, off, ok);
        if (!ok)
            return std::nullopt;
        grid.setElevation(grid.cellOf(idx), e);
    }

    // Flag plane
    for (size_t idx = 0; idx < cellCount; ++idx) {
        uint32_t f = getU32(bytes, off, ok);
        if (!ok)
            return std::nullopt;
        grid.setFlags(grid.cellOf(idx), f);
    }

    // Exact-size requirement: no trailing garbage.
    if (off != bytes.size())
        return std::nullopt;

    return grid;
}

} // namespace bro::tile
