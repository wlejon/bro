#pragma once

// Is there a GL entry-point table at all?
//
// glad resolves the entire table in one shot (gladLoadGL, called from
// platform/sdl_window.cpp when the window's GL context comes up). On the
// CPU-raster path it never runs: `--no-gpu`, or a headless boot whose SDL video
// init failed and fell back to RasterRenderer (see engine_init.cpp). There every
// glad_gl* pointer stays null, so the first GL call the 3D renderer makes is a
// jump to address 0 — a segfault with a backtrace that names a shader compile
// and says nothing about the missing context.
//
// The engine's own guard is `gl_ != nullptr`, but the scene module can't see
// Engine, and the paths that reach GL from JS (getContext('scene') → flush() →
// render, reflection-probe capture) are numerous enough that a single funnel
// check is worth more than a guard at each caller. Entry points that JS can
// reach without a context check this first.

#include <glad/gl.h>

namespace bro::scene {

/// True once glad has resolved the GL function table, i.e. a real GL context
/// exists. Cheap enough to call per frame (a load-and-compare).
inline bool glFunctionsLoaded() {
    return glad_glCreateShader != nullptr;
}

}  // namespace bro::scene
