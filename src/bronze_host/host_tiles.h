#pragma once

namespace bro::bronze_host {

/// `bro.tiles`: tile caches filled on demand by world code
/// (docs/tiles-api.js) over the engine's GPU memory (host_kernel_gpu.h) and
/// tilecache::TileTree. Mounted with bro.kernel, after installKernelGpu.
void installTiles();

} // namespace bro::bronze_host
