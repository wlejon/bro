#pragma once

namespace bro::tilecache {

/// tiles.bk, the brokernel module that reads a tile cache from a shader or
/// a kernel (its `lookup`, a point's tile, the cells to filter): what a
/// world registers as `tiles` (bro.tiles.module).
const char* tilesModuleSource();

} // namespace bro::tilecache
