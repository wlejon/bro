// The GPU cell pass (scene_world_cells_pass.cpp, world_cells_cull.comp): the
// words of its buffers, for C++ and GLSL alike. Every offset counts 32-bit
// words (a float as its bits).
//
//   binding 0  CELLS: one provider's tops and leaves (WorldCells::flatten),
//              rebuilt when its content or bounds change
//   binding 1  PARAMS: this frame's views (the camera first), their pixel
//              scales, and the provider's band thresholds
//   binding 2  OUT: the cells in view this frame, and the dispatch over
//              their items (WC_O_*), which the provider's passes read
//   binding 3  ORIGINS: this frame's eye-relative origin of every leaf
//              (WC_ORIGIN_WORDS a leaf, in the leaves' order), computed in
//              double on the CPU

#ifndef BRO_SCENE_WORLD_CELLS_LAYOUT_H
#define BRO_SCENE_WORLD_CELLS_LAYOUT_H

#define WC_GROUP_SIZE           256
#define WC_TOP_LEAVES           64    // the most leaves a top holds (CellGrid::topLevel 3: 8 x 8)
#define WC_MAX_VIEWS            33    // the camera and 32 shadow views (PP_MAX_TILES, the plants plan's)
#define WC_ORIGIN_WORDS         4     // xyz, 0

// --- CELLS: the header, the tops, then the leaves ---
#define WC_H_TOPS               0     // uint: the tops
#define WC_H_LEAVES             1     // uint: the leaves
#define WC_H_TOP_AT             2     // the first top's first word
#define WC_H_LEAF_AT            3     // the first leaf's
#define WC_H_WORDS              8
// A top or a leaf: its bounds, its run (a top's of leaves, a leaf's of the
// provider's items) and its key (CellKey::packed).
#define WC_CELL_WORDS           12
#define WC_CELL_MIN             0     // xyz
#define WC_CELL_MAX             3     // xyz
#define WC_CELL_FIRST           6     // uint
#define WC_CELL_COUNT           7     // uint
#define WC_CELL_KEY             8     // uint x2: lo, hi

// --- PARAMS ---
#define WC_P_VIEWS              0     // uint: the views (the camera first)
#define WC_P_ITEM_GROUP         1     // uint: items a workgroup of the provider's dispatch
#define WC_P_EYE                4     // xyz: the camera's eye (distances, for every view's scale)
#define WC_P_BAND_NEAR_PX       8     // the provider's: a cell this many pixels or more (camera) is near
#define WC_P_BAND_FAR_PX        9     // one under this many is far; between, mid
#define WC_P_SCALES             16    // per view, WC_SCALE_WORDS: how a size in metres becomes pixels
#define WC_P_PLANES             (WC_P_SCALES + WC_SCALE_WORDS * WC_MAX_VIEWS)   // per view, six planes (24)
#define WC_P_WORDS              (WC_P_PLANES + 24 * WC_MAX_VIEWS)
// A view's scale: pixels = metres * focal / distance (a perspective
// camera), or metres / (texel + texelPerMetre * distance) (a shadow view:
// its texel's size, constant when ortho), the distance the camera eye's.
#define WC_SCALE_WORDS          4
#define WC_SCALE_FOCAL          0     // the focal length in pixels (pixels a metre at a metre); 0 for a tile
#define WC_SCALE_TEXEL          1     // a tile's texel in metres
#define WC_SCALE_TEXEL_PER_METRE 2    // its growth a metre of distance (0 ortho)

// --- OUT: the header, then the runs below ---
#define WC_O_DISPATCH           0     // x, 1, 1: the items' workgroups (at least one)
#define WC_O_CELLS              4     // uint: the cells in view (of any view)
#define WC_O_ITEMS              5     // uint: their items
#define WC_O_TOPS               6     // uint: the tops in view
#define WC_O_BAND_CELLS         8     // per band (near, mid, far): its cells
#define WC_O_BAND_ITEMS         12    // per band: their items
#define WC_O_VIEW_CELLS         16    // per view: the cells it sees (WC_MAX_VIEWS)
#define WC_O_TOP_DISPATCH       52    // x, 1, 1: the leaf step's workgroups (a top in view each)
#define WC_O_HEADER             64
// The runs, each with room for every top or leaf (wcRun* in the shader):
//   the cells in view, in the leaves' order (WC_V_WORDS a cell, H_LEAVES)
//   the tops in view, in order (WC_T_WORDS a top, H_TOPS)
//   the leaf step's cells, a top's in view from its first leaf's place
//     (WC_V_WORDS a cell, H_LEAVES), gathered into the first run
//   each band's cells in view: indices into the first run (H_LEAVES a band)
//   each leaf's projected size in pixels a view (a float, P_VIEWS a leaf,
//     by leaf: written for a leaf in view, 0 for a view that does not see it)
#define WC_V_WORDS              12
#define WC_V_LEAF               0     // uint: the leaf
#define WC_V_FIRST              1     // uint: its items' first (the provider's order)
#define WC_V_COUNT              2     // uint: its items
#define WC_V_PREFIX             3     // uint: the items of the cells in view before it
#define WC_V_MASK               4     // uint x2: the views that see it (bit v)
#define WC_V_BAND               6     // uint: WC_BAND_* by the provider's thresholds
#define WC_V_BAND_RANK          7     // uint: its place among its top's cells of its band
#define WC_V_ORIGIN             8     // xyz: its origin relative to the eye (ORIGINS')
#define WC_T_WORDS              16
#define WC_T_TOP                0     // uint
#define WC_T_MASK               1     // uint x2: the views that see it
#define WC_T_CELLS              3     // uint: its cells in view (the leaf step's)
#define WC_T_ITEMS              4     // uint: their items
#define WC_T_BAND_CELLS         5     // uint x3: its cells in view a band
#define WC_T_CELL_PREFIX        8     // uint: the cells in view of the tops before it
#define WC_T_ITEM_PREFIX        9     // uint: their items
#define WC_T_BAND_PREFIX        10    // uint x3: their cells a band

#define WC_BAND_NEAR            0u
#define WC_BAND_MID             1u
#define WC_BAND_FAR             2u
#define WC_BANDS                3

#endif
