#pragma once
// The input region of a click-through window (bro.window.clickThrough): the
// part of the window where the page would take a pointer event, as
// rectangles in window coordinates. Everywhere else input passes through to
// whatever lies beneath the window.
//
// The region is derived from the page's own hit test, so authors steer it
// with CSS alone: a point takes input when the hit test lands on an element
// other than <html> and <body> (no box there, or `pointer-events: none` all
// the way down, passes through). See Engine::takesPointerAt.
//
// computeHitRegion turns that point query into rectangles. The boxes' edges
// cut the window into cells inside which the answer cannot change (for
// boxes without rotation, where every hit-test boundary is a box edge); one
// hit test at each cell's centre answers for the whole cell, and the cells
// that take input are merged into rows and then into rectangles.

#include "platform/window.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace bro::engine {

/// The default bound on how many cells one computation tests.
inline constexpr size_t kHitRegionMaxCells = 16384;

/// The rects (window coordinates, cut to `width` x `height`) where
/// `takesInput(x, y)` holds, from the cell grid `xEdges` x `yEdges` (any
/// order, duplicates allowed; 0 and the window's edges are added). Rects are
/// rounded outward to whole units. With `candidates`, a cell whose centre
/// lies in none of them is taken not to take input without asking (the
/// boxes anything could be hit in). Returns false, leaving `out` empty, when
/// the grid has more than `maxCells` cells.
bool computeHitRegion(int width, int height, std::vector<float> xEdges, std::vector<float> yEdges,
                      const std::function<bool(float x, float y)>& takesInput, std::vector<platform::WindowRect>& out,
                      size_t maxCells = kHitRegionMaxCells,
                      const std::vector<platform::WindowRect>* candidates = nullptr);

/// Whether (x, y) lies in one of `rects`.
bool regionContains(const std::vector<platform::WindowRect>& rects, float x, float y);

}  // namespace bro::engine
