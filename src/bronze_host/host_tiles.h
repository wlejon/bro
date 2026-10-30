#pragma once

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// `bro.tiles`: tile caches filled on demand by world code
/// (docs/tiles-api.js) over the engine's GPU memory (host_kernel_gpu.h) and
/// tilecache::TileTree. Mounted with bro.kernel, after installKernelGpu.
/// The live caches' wanted tiles not yet produced report to the engine's
/// settle state (Engine::addPendingWorkSource), once an engine.
void installTiles(engine::Engine& engine);

} // namespace bro::bronze_host
