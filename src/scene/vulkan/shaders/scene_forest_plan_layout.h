// The forest's GPU planner (scene_forest_gpu_plan.cpp, the forest_plan_*.comp
// stages): the words its buffers hold, for C++ and GLSL alike, as
// scene_forest_layout.h is for the node's data. Every offset counts 32-bit
// words (a float as its bits) from the start of its block.
//
// The planner's buffers (set 0 of every forest_plan_* stage):
//   binding 0  STATIC: the node's trees and specimens as the plan reads them,
//              built when its data or transform changes (FP_ST_*)
//   binding 1  PARAMS: this frame's view, Lods, shadow tiles and the work
//              buffer's runs (FP_P_*), in the frame's upload memory
//   binding 2  WORK: the node's working words this frame (its state, FP_N_*,
//              then the runs FP_P_WORK names), kept from frame to frame
//   binding 3  SHARED: the budget's search and the settled details (FP_G_*),
//              one for every node planned under one budget
//   binding 4  ENTRIES: the draw entries (FOREST_E_*), FP_P_ENTRY_CAP of them
//   binding 5  ARGS: the draws' indirect commands (FP_A_*)
//   binding 6  CELLS: the world cell pass's output for the node's cells
//              (world_cells_layout.h's OUT), read by the measure stage
//   binding 7  CARDS: a word a cell of the node, nonzero where a far card
//              stands for the cell's trees this frame (FP_F_CARDS set), so
//              the camera leaves them out (SceneFrame::forestCellSkip)
//
// The trees are planned in the node's cell order (ForestNode::cellOrder):
// the measure stage walks the trees of the cells some view sees, or with
// BRO_WORLD_CELLS=0 every tree, in that order, and the stages after it walk
// what it measured (FP_N_MEASURED: their dispatches from words the measure
// stage writes). A tree is its instance index everywhere else (the camera's
// measure of it, an item, an entry), so the order is all the cells change.

#ifndef BRO_SCENE_FOREST_PLAN_LAYOUT_H
#define BRO_SCENE_FOREST_PLAN_LAYOUT_H

// --- Sizes ---
#define FP_GROUP_SIZE           256   // threads a workgroup, and items an item block
#define FP_SCAN_BLOCK           1024  // elements a scan workgroup takes (4 a thread)
#define FP_GROUPS               14    // the drawer's groups (forest::Group)
#define FP_COLUMNS              16    // a place pass's columns: the groups' entries, then the
                                      // leaf groups' leaves (LeafNear, LeafCard; a tile's LeafCard)
#define FP_MAX_TILES            32
#define FP_MAX_VIEWS            (1 + FP_MAX_TILES)   // the camera, then each shadow tile
#define FP_POSED_GROUPS         (2 + FP_MAX_TILES)   // the camera's LeafNear and LeafCard, then each
                                      // tile's LeafCard casters (view v's at 1 + v)
#define FP_VIEW_SHIFT           26    // an item: its tree | its view << FP_VIEW_SHIFT
#define FP_TREE_MASK            ((1u << FP_VIEW_SHIFT) - 1u)
#define FP_NO_ITEM              0xffffffffu
#define FP_ALL_SEGMENTS         0xffffffffu   // a bark class measured at the tree's nearest point
#define FP_ROUNDS               10    // the budget's search asks at most this many details
#define FP_MAX_DETAIL           8.0   // fitDetail's kMaxDetail
// The camera's trees go nearest first by the top FP_SORT_BITS of their
// distance's float bits (the exponent and 7 bits of the mantissa: within
// 1%, as the CPU orders its crowns), FP_SORT_DIGIT_BITS a pass.
#define FP_SORT_BITS            16
#define FP_SORT_DIGIT_BITS      4
#define FP_SORT_DIGITS          16
#define FP_SORT_PASSES          4
#define FP_SORT_UNSEEN          0xffffu   // the key of a tree the camera does not see (last)
// A scan whose length the GPU sets (the measured trees', the item blocks'):
// its levels' counts in WORK words and their dispatches in ARGS, at most
// FP_SCAN_LEVELS levels (FP_SCAN_BLOCK^4 elements). A scan's push.count with
// FP_SCAN_COUNT_WORD set names the WORK word holding its count.
#define FP_SCAN_LEVELS          4
#define FP_SCAN_COUNT_WORD      0x80000000u

// The camera's Clumps and Crowns draws come in FP_BATCHES batches, nearest
// first, each a share of the group's entries: batch b is entries
// [floor(n * f_b), floor(n * f_(b+1))) with f = 0, FP_BATCH_END_0,
// FP_BATCH_END_1, 1 (MeshDraw::indirectBatches).
#define FP_BATCHES              3
#define FP_BATCH_END_0          0.05
#define FP_BATCH_END_1          0.25

// --- STATIC: the header, then the runs it names ---
#define FP_ST_TREES             0     // first word of the trees (FP_TREE_WORDS each)
#define FP_ST_SPECIMENS         1     // of the specimens (FP_SPEC_WORDS each)
#define FP_ST_KEEP              2     // each segment's keep radius (ForestNode::keepRadius)
#define FP_ST_AREA              3     // the wood's area before each segment, one more (woodArea)
#define FP_ST_CLUMPS            4     // 4 a clump: centre xyz, radius (clumpRecords' first four)
#define FP_ST_CLUMP_COUNT       5
#define FP_ST_TWIGS             6     // 2 a clump: stems, pieces (clumpTwigs)
#define FP_ST_TWIG_COUNT        7
#define FP_ST_SEGMENTS          8     // 8 a segment: from xyz, 0, to xyz, 0 (segmentRecords)
#define FP_ST_SEGMENT_COUNT     9
#define FP_ST_ORDER             10    // the trees in cell order (ForestNode::cellOrder: instance indices)
#define FP_ST_HEADER            16

// A tree: its world bounds padded by the sway's reach (ForestCull::bounds:
// the static data is rebuilt when the reach moves), and its instance in
// the node's space.
#define FP_TREE_WORDS           16
#define FP_TREE_MIN             0     // xyz
#define FP_TREE_MAX             3     // xyz
#define FP_TREE_POSITION        6     // xyz
#define FP_TREE_SCALE           9
#define FP_TREE_COS_YAW         10
#define FP_TREE_SIN_YAW         11
#define FP_TREE_SPECIMEN        12    // uint

// A specimen: what treeLod, casterLod and addTree read of it.
#define FP_SPEC_WORDS           24
#define FP_SPEC_SEG_START       0     // uint, the eight
#define FP_SPEC_SEG_COUNT       1
#define FP_SPEC_CLUMP_START     2
#define FP_SPEC_CLUMP_COUNT     3
#define FP_SPEC_LEAF_START      4
#define FP_SPEC_LEAF_COUNT      5
#define FP_SPEC_BLOOM_START     6
#define FP_SPEC_BLOOM_COUNT     7
#define FP_SPEC_CROWN           8     // xyz
#define FP_SPEC_CROWN_RADIUS    11
#define FP_SPEC_CROWN_UP        12
#define FP_SPEC_CLUMP_RADIUS    13    // meanClumpRadius
#define FP_SPEC_TRUNK_RADIUS    14
#define FP_SPEC_BLOOM_EXTENT    15    // bloomSize * ForestNode::bloomExtent(bloomForm)
#define FP_SPEC_PINNATE         16    // uint 0 / 1
#define FP_SPEC_WOOD_SEEN       17    // ForestNode::woodSeen
#define FP_SPEC_BARK_OVER_LEAF  18    // rgb (ForestNode::barkOverLeaf)

// --- PARAMS ---
#define FP_P_EYE                0     // xyz
#define FP_P_PX_AT_METRE        3
#define FP_P_NODE_SCALE         4
#define FP_P_REACH              5
#define FP_P_PERSPECTIVE        6     // uint
#define FP_P_TREES              7     // uint: the node's trees
#define FP_P_TILES              8     // uint: the shadow tiles planned
#define FP_P_LEAF_LENGTH        9
#define FP_P_LEAF_VERTS         10    // uint
#define FP_P_LEAF_COPIES        11    // uint
#define FP_P_CARD_VERTS         12    // uint
#define FP_P_FLAGS              13    // uint: FP_F_*
#define FP_P_BLOOM_BASE         14    // uint
#define FP_P_ENTRY_CAP          15    // uint: the entries buffer's room
#define FP_P_POSE_CAP           16    // uint: the pose buffer's room (poses)
#define FP_P_ITEMS              17    // uint: the item runs' room (FP_P_ALIGNED a view)
#define FP_P_ALIGNED            18    // uint: the trees rounded up to FP_SCAN_BLOCK (the sort's blocks)
#define FP_P_ITEM_GROUPS        19    // uint: FP_P_ITEMS / FP_GROUP_SIZE (the item blocks' room)
#define FP_P_WORLD              20    // the world matrix's three rows (12)
#define FP_P_INV_WORLD          32    // its inverse's (12)
#define FP_P_FRUSTUM            44    // the camera's six planes: normal xyz, d (24)
#define FP_P_LOD                68    // the camera's Lod (FP_LOD_*)
#define FP_P_SHADOW_LOD         84    // the casters' Lod
#define FP_P_WORK               100   // the work runs' first words (FP_W_*)
#define FP_P_SEARCH             116   // the budget's search (FP_Q_*)
#define FP_P_DRAWS              128   // per group, the camera's then the casters': kind | instances << 8, count (64)
#define FP_P_TILE               192   // per tile, FP_TILE_WORDS
#define FP_P_WORDS              (FP_P_TILE + FP_MAX_TILES * FP_TILE_WORDS)

#define FP_F_CORES              1u
#define FP_F_CLUSTERS           2u
#define FP_F_TWIGS              4u
#define FP_F_NEEDLES            8u
#define FP_F_CARDS              16u   // CARDS holds the far cards' cells (the plan walks the cells)

// A Lod (ForestNode::Lod's fields the plan reads).
#define FP_LOD_LEAF_PIXELS      0
#define FP_LOD_CARD_WIDENING    1
#define FP_LOD_CLUMP_FROM       2
#define FP_LOD_CLUMP_TO         3
#define FP_LOD_CROWN_FROM       4
#define FP_LOD_CROWN_TO         5
#define FP_LOD_BARK_PIXELS      6
#define FP_LOD_BARK_SIDES_PIXELS 7
#define FP_LOD_SHADOW_KEEP      8
#define FP_LOD_RANGE            9
#define FP_LOD_BARK_KEEP_PIXELS 10
#define FP_LOD_BARK_FINE_SIDES  11
#define FP_LOD_LEAVES_RANGE     12
#define FP_LOD_LEAVES_BAND      13
#define FP_LOD_WORDS            16

// A shadow tile: its frustum's planes, its texel's size (ShadowTilePlan).
#define FP_TILE_WORDS           28
#define FP_TILE_FRUSTUM         0     // 24
#define FP_TILE_TEXEL_CONST     24
#define FP_TILE_TEXEL_PER_METRE 25
#define FP_TILE_ORTHO           26    // uint

// The work runs (FP_P_WORK + these: each one's first word in WORK).
#define FP_W_CAMERA             0     // FP_CAM_WORDS a tree: the camera's measure of it (a tree
                                      //   measured this frame's; another's is stale and unread)
#define FP_W_FLAGS              1     // tiles x measured + 1: in a tile's view, then where in the scan
#define FP_W_ITEMS              2     // FP_P_ITEMS: each view's trees, a block-aligned run a view
#define FP_W_KEYS_A             3     // the sort's keys and trees, twice (FP_P_ALIGNED each)
#define FP_W_VALS_A             4
#define FP_W_KEYS_B             5
#define FP_W_VALS_B             6
#define FP_W_HIST               7     // FP_SORT_DIGITS x blocks: the sort's digit counts
#define FP_W_FINE               8     // FP_P_ITEMS: each item's cost at detail 1
#define FP_W_SUMS               9     // FP_COLUMNS x item groups + 1: the place pass's counts
#define FP_W_SCRATCH            10    // the scans' block sums
#define FP_W_VARY               11    // FP_P_ITEMS: the items whose cost varies with the detail (the
                                      //   search's later rounds walk these alone), in no order
#define FP_W_TREE_OF            12    // FP_P_ALIGNED: each measured place's tree, in cell order
#define FP_W_CLASSIFY           13    // FP_CLASSIFY_WORDS a tree: the bark classes' jobs (the trees
                                      // with segments to measure, as measured, in any order)
#define FP_W_RUNS               14

// A bark classes' job (FP_W_CLASSIFY).
#define FP_CLASSIFY_WORDS       8
#define FP_CJ_TREE              0
#define FP_CJ_SEG               1     // its specimen's first segment
#define FP_CJ_COUNT             2     // the segments to measure
#define FP_CJ_EYE               3     // vec3: the eye in its specimen's space
#define FP_CJ_UNIT              6     // float: its metres a unit

// The camera's measure of a tree (ForestNode::SeenTree and the bark's classes).
#define FP_CAM_WORDS            8
#define FP_CAM_SEEN             0     // uint: 1 seen
#define FP_CAM_PX               1
#define FP_CAM_BARK_PX          2
#define FP_CAM_DIST             3
#define FP_CAM_FAR              4
#define FP_CAM_CURVE_END        5     // uint (FP_ALL_SEGMENTS: at the nearest point)
#define FP_CAM_FINE_END         6
#define FP_CAM_NEAR_END         7

// The search (the camera's, then the shadow's: FP_Q_* + 4 * search).
#define FP_Q_ON                 0     // uint: a budget to fit
#define FP_Q_LIMIT              1     // uint64 (lo, hi): the most vertices that fit
#define FP_Q_HOLD               3     // uint: keep the settled detail (the shadow's, a frame holding a tile)
#define FP_Q_SNAP               8     // uint: the settle snaps (a jump, a new budget, a new path)
#define FP_Q_STEP               9     // float: the settle's rate this frame
#define FP_Q_SLOT               10    // uint: the settled details' slot in SHARED

// --- WORK's first words: the node's state this frame (zeroed as it begins) ---
#define FP_N_VIEW_COUNT         0     // per view: its trees
#define FP_N_VIEW_BASE          40    // its first item (block-aligned)
#define FP_N_VIEW_START         80    // its first place in the flags' scan
#define FP_N_STATS              128   // FP_S_*: copied to the read-back
#define FP_N_MEASURED           160   // the trees measured (in the cells in view, or all)
#define FP_N_ITEM_STRIDE        161   // the measured rounded up to FP_GROUP_SIZE: the items stage's run a view
#define FP_N_ITEM_GROUPS        162   // the item blocks in use (the place counts' column)
#define FP_N_SCAN_FLAGS         164   // the flags' scan: each level's count (FP_SCAN_LEVELS)
#define FP_N_SCAN_SUMS          168   // the place counts' scan: the same
#define FP_N_VARY_COUNT         180   // the items in FP_W_VARY
#define FP_N_CLASSIFY_JOBS      181   // the jobs in FP_W_CLASSIFY
#define FP_N_POSED              184   // per posed group (FP_POSED_GROUPS): 1 posed, 0 inline
#define FP_N_POSE_BASE          (FP_N_POSED + FP_POSED_GROUPS)   // per posed group: its first pose slot
#define FP_N_SORT_TOTALS        256   // per sort pass, per digit: the measured's keys of it
#define FP_N_WORDS              (FP_N_SORT_TOTALS + FP_SORT_PASSES * FP_SORT_DIGITS)
#if FP_N_POSE_BASE + FP_POSED_GROUPS > FP_N_SORT_TOTALS
#error "the node's posed groups run past WORK's first words"
#endif

// The stats (ForestNode::Stats), as read back.
#define FP_S_TREES              0     // 3
#define FP_S_CULLED             3
#define FP_S_LEAVES             4     // uint64 (lo, hi)
#define FP_S_BLOOMS             6     // uint64
#define FP_S_VERTICES           8     // uint64
#define FP_S_SHADOW_VERTICES    10    // uint64
#define FP_S_ENTRIES            12    // every entry asked for (past the room too)
#define FP_S_DRAWS              13
#define FP_S_DETAIL             14    // float
#define FP_S_SHADOW_DETAIL      15    // float
#define FP_S_POSES              16    // the leaves the posed groups asked to pose
#define FP_S_MEASURED           17    // the trees measured (FP_N_MEASURED)
#define FP_S_WORDS              32

// --- SHARED ---
#define FP_G_SUMS               0     // [search][round] uint64: each ask's vertices (2 x FP_ROUNDS x 2)
#define FP_G_FIXED              40    // [search] uint64: round 1's trees whose cost does not vary
#define FP_G_DETAIL             44    // float: the settled detail, then the shadow's
#define FP_G_POSE_CURSOR        46    // the pose slots handed out so far
#define FP_G_POSE_WANTED        47    // those the posed groups would take with kMaxPoses' room
#define FP_G_FRAME_WORDS        48    // the words above, zeroed as a budget's plan begins
#define FP_G_TABLE              64    // 3 x 64 floats: the bisection's asks (FP_TABLE_*)
#define FP_G_SETTLED            256   // per slot: its detail and shadow detail as last settled
#define FP_G_SLOTS              256
#define FP_G_WORDS              (FP_G_SETTLED + 2 * FP_G_SLOTS)

// --- ARGS: per view, FP_GROUPS + 2 slots (the posed groups' inline draws
// at FP_GROUPS, FP_GROUPS + 1), each 1 + FP_BATCHES commands of 5 words (an
// indexed command; an unindexed one in its first 4) ---
#define FP_A_SLOTS_PER_VIEW     16
#define FP_A_COMMAND_WORDS      5
#define FP_A_SLOT_WORDS         (FP_A_COMMAND_WORDS * (1 + FP_BATCHES))
#define FP_A_POSE               (FP_MAX_VIEWS * FP_A_SLOTS_PER_VIEW * FP_A_SLOT_WORDS)   // per posed group:
                                      // a dispatch (x, 1, 1) and its first entry
#define FP_A_ITEM_DISPATCH      (FP_A_POSE + 4 * FP_POSED_GROUPS)   // the item passes' dispatch: the item blocks in use, 1, 1
#define FP_A_VARY_DISPATCH      (FP_A_ITEM_DISPATCH + 4)   // the search's later rounds': FP_W_VARY's blocks
#define FP_A_SORT_DISPATCH      (FP_A_VARY_DISPATCH + 4)   // the sort's: the measured's FP_SCAN_BLOCK blocks
#define FP_A_FILL_DISPATCH      (FP_A_SORT_DISPATCH + 4)   // the items stage's: each view's run of the measured
#define FP_A_SCAN_FLAGS         (FP_A_FILL_DISPATCH + 4)   // the flags' scan: each level's blocks (4 a level)
#define FP_A_SCAN_SUMS          (FP_A_SCAN_FLAGS + 4 * FP_SCAN_LEVELS)   // the place counts' scan: the same
#define FP_A_WORDS              (FP_A_SCAN_SUMS + 4 * FP_SCAN_LEVELS)

// A group's draw (FP_P_DRAWS): its command's kind.
#define FP_DRAW_INDEXED         0u    // instanced, indexed: an entry an instance
#define FP_DRAW_INSTANCED       1u    // instanced, unindexed
#define FP_DRAW_CROWNS          2u    // unindexed, FOREST_CARD_VERTICES an entry

#endif
