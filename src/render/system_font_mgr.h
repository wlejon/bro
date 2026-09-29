#pragma once

#include <include/core/SkFontMgr.h>
#include <include/core/SkRefCnt.h>

#include <cstdint>

namespace bro::render {

// One platform font manager for the whole process (DirectWrite on Windows,
// CoreText on Apple, fontconfig elsewhere), shared by every renderer and the
// canvas worker. SkFontMgr and the typefaces it hands out are thread-safe.
//
// Building it is not cheap and not bounded: DirectWrite's system collection
// and its fallback map talk to the font cache service and stat font files,
// which on some machines stalls for most of a second. The engine starts the
// build on a worker thread at construction (prewarmSystemFontMgr), long before
// the first layout, so that stall overlaps window creation and page compile
// instead of blocking the first frame. systemFontMgr() returns the finished
// manager, waiting only for whatever of the warm-up is still in flight.
void prewarmSystemFontMgr();
SkFontMgr* systemFontMgr();

// Logs one platform font lookup that took long enough to cost a frame (a
// family match, a per-character fallback), with what it looked up: a stall
// in the font system is otherwise invisible inside the layout that paid it.
void noteFontLookup(const char* what, const char* name, int32_t codepoint, double ms);

} // namespace bro::render
