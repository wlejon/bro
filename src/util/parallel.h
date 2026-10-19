#pragma once
// A shared pool of worker threads for the frame's data-parallel work (a
// forest's per-tree planning): parallelChunks runs a function over chunk
// indices on the pool and the calling thread, returning once all have run.

#include <cstdint>
#include <functional>

namespace bro::util {

/// Run `fn(chunk)` for every chunk in [0, chunks), across the pool's workers
/// and the calling thread, in any order; returns when all have run. One job
/// runs at a time: a call from inside a chunk, or while another thread's
/// job runs, runs its chunks inline. `fn` must not throw.
void parallelChunks(uint32_t chunks, const std::function<void(uint32_t)>& fn);

/// The threads a job spreads over (the workers and the caller).
uint32_t parallelWidth();

}  // namespace bro::util
