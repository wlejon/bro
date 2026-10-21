// Cached shadow pages (docs/scene-world-architecture.md §8): the numbers the
// CPU's paging (scene/world/shadow_pages.h), the marking pass
// (shadow/shadow_mark.comp) and every lit shader's lookup
// (common/scene_shadow_pages.glsl) agree on, for C++ and GLSL alike.
//
// A virtual map is a square of pages of SP_PAGE texels. The sun's shadow is
// a clipmap: SP_MAX_LEVELS levels, each a window of SP_WINDOW x SP_WINDOW
// pages fixed in light space and snapped in whole pages, each level's texel
// twice the one before. A spot light's map, and each of a point light's six
// faces, is a mip chain of SP_LOCAL_MIPS levels, SP_LOCAL x SP_LOCAL pages
// at its finest and one page at its coarsest.
//
// The page table is one array of entries; each map owns a run of it: a
// clipmap level SP_LEVEL_ENTRIES (its window, addressed toroidally: page
// (x, y) of the absolute grid at entry (y mod SP_WINDOW) * SP_WINDOW +
// (x mod SP_WINDOW)), a local map SP_LOCAL_ENTRIES (its mips, finest first,
// row-major). An entry is two words: SP_RESIDENT plus its physical page's
// index, and SP_RESIDENT plus where its overlay lies in the scratch target
// when what moves over it was drawn this frame. The marking pass's requests
// are a bit an entry, in the same order.
//
// A physical page is SP_STRIDE texels a side in the pool: its SP_PAGE
// texels and a border of SP_BORDER drawn from the neighbours' casters, so a
// filter reaching past the page's edge reads what is there and no seam shows.

#ifndef BRO_SCENE_SHADOW_PAGES_LAYOUT_H
#define BRO_SCENE_SHADOW_PAGES_LAYOUT_H

#define SP_PAGE                 128
#define SP_BORDER               4
#define SP_STRIDE               (SP_PAGE + 2 * SP_BORDER)
#define SP_WINDOW               64                      // a clipmap level's pages a side
#define SP_LEVEL_ENTRIES        (SP_WINDOW * SP_WINDOW)
#define SP_MAX_LEVELS           16
#define SP_LOCAL                8                       // a local map's finest mip, pages a side
#define SP_LOCAL_MIPS           4                       // 8, 4, 2, 1 pages a side
#define SP_LOCAL_ENTRIES        85                      // 64 + 16 + 4 + 1
#define SP_MAX_MAPS             32                      // the lighting block's maps: the sun's levels, local faces
#define SP_MAX_VIEWS            32                      // a frame's drawn rects (the plants' plan's views)

// A pixel marks its page at the level its footprint wants and at
// SP_GUARD_LEVELS coarser: the guard, a page that covers the eye's moves for
// longer, and the top of what it needs.
#define SP_GUARD_LEVELS         3

#define SP_RESIDENT             0x80000000u
#define SP_PHYSICAL_MASK        0xFFFFu
// An overlay's word: its page's first texel in the scratch target, x in the
// low SP_OVERLAY_BITS, y in the next.
#define SP_OVERLAY_BITS         12
#define SP_OVERLAY_MASK         0xFFFu

// The finest mip's first entry of mip m in a local map's run.
#define SP_LOCAL_MIP_FIRST(m)   ((m) == 0 ? 0 : (m) == 1 ? 64 : (m) == 2 ? 80 : 84)

// A map's kind (ShadowMapGpu info.w).
#define SP_KIND_CLIPMAP         0
#define SP_KIND_LOCAL           1

#endif
