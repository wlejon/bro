#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.worldgen` namespace — learned terrain generation via
// brodiffusion's WorldPipeline (a port of xandergos/terrain-diffusion).
//
// This is a replacement for noise, not a decoration on top of it. Three
// magnitude-preserving UNets produce an infinite, deterministic, randomly
// accessible world: elevation in METRES, one cell per 30 m, with coherent
// drainage networks, coastlines and mountain ranges that FBm cannot produce at
// any parameter setting. The world is a pure function of (seed, position), so
// any region can be read in any order and reads agree where they overlap.
//
//   bro.worldgen.loadWorld(dir, { seed, onReady, onError })  -> AsyncHandle
//   world.elevation(i1, j1, i2, j2, { onDone, onError })      -> AsyncHandle
//   world.elevationSync(i1, j1, i2, j2)                       -> result object
//
// Both elevation forms return { width, height, cellSize, data } where `data` is
// a row-major Float32Array of height*width metres. Bounds are in CELLS and are
// half-open: [i1, i2) rows (north-south), [j1, j2) columns (west-east). They may
// be negative — the origin is just the seed's reference point, not a corner.
//
// SIZE THE REQUEST GENEROUSLY. Cost is dominated by a fixed overhead, not by
// area: 256x256 takes ~2.3 s and 2048x2048 ~14.6 s, so the large request is
// roughly 25x cheaper per cell. Ask for one big tile and sample it, rather than
// one request per consumer chunk.
//
// TILES COMPOSE EXACTLY, BY DEFAULT. The reconstruction pads outward and crops,
// but that pad is finite, so the outermost cells of any request are built from
// truncated support: measured on the 30 m checkpoint, a region read alone
// differs from the same region read inside a larger one by 0.18 m at the very
// edge, decaying to bit-exactly zero four cells in. Both forms therefore
// over-request by `opts.margin` cells (default 8) and crop, so independently
// generated neighbouring tiles agree exactly where they meet and a chunked
// consumer shows no seam. Pass `margin: 0` for the pipeline's raw output — that
// is what the C++ parity gates compare against, and it is the only reason to.
//
// elevation() runs on a background thread (the engine's async-job runner), so
// the frame loop keeps running; elevationSync() blocks the calling thread and
// exists for headless tests and workers. A pipeline serves one request at a
// time: its tile cache is not thread-safe, so a call made while another is in
// flight throws rather than racing.
void installWorldgenBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupWorldgenBindings(JSContext* ctx);

}  // namespace bro::js
