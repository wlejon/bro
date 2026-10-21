// The plants' GPU plan (scene_plants_gpu_plan.cpp, plants_plan.comp): the
// words of its buffers, for C++ and GLSL alike. Every offset counts 32-bit
// words (a float as its bits).
//
//   binding 0  STATIC: the node's header, species and plants in cell order
//              (PlantsNode::gpuData; scene_plants.glsl's set 3 binding 1)
//   binding 1  PARAMS: this frame's camera, shadow views and the node's
//              detail params (PP_P_*)
//   binding 2  CELLS: the world cell pass's output for the node's cells
//              (world_cells_layout.h), whose items in view the plan walks
//   binding 3  WORK: the plan's counts, cursors and places (PP_N_*), one
//              copy, zeroed each frame
//   binding 4  CHUNKS: the chunk records the draws and the poses read
//              (scene_plants.glsl's set 3 binding 2), this frame slot's
//   binding 5  ARGS: the pose stage's dispatch and the draws' indirect
//              commands (PP_A_*), this frame slot's
//
// A SLOT is where a view's chunks go: the camera's PP_BANDS distance bands
// (nearest first), then each shadow view's one.

#ifndef BRO_SCENE_PLANTS_PLAN_LAYOUT_H
#define BRO_SCENE_PLANTS_PLAN_LAYOUT_H

#define PP_GROUP_SIZE           64
#define PP_BANDS                48    // the camera's distance bands: 4 an octave of 1 + the distance (m)
#define PP_MAX_TILES            32
#define PP_SLOTS                (PP_BANDS + PP_MAX_TILES)
#define PP_LEAVES_PER_CHUNK     32    // a leaf chunk's elements (scene_plants.glsl's)
#define PP_LIMBS_PER_CHUNK      8     // a wood chunk's tubes
#define PP_LEAF_INDICES         12    // indices an element (the drawer's leafIndices)
#define PP_TUBE_VERTS           36    // vertices a tube (PLANT_TUBE_VERTS)
#define PP_MAX_LOBES            48

// --- PARAMS ---
#define PP_P_TILES              0     // uint: the shadow views drawn (0: the node casts none)
#define PP_P_CHUNK_CAP          1     // uint: the chunk records CHUNKS holds
#define PP_P_LEAF_CAP           2     // uint: the leaf chunks the poses hold (a leaf chunk's index under it)
#define PP_P_EYE                4     // xyz
#define PP_P_FOCAL              7     // pixels a metre at a metre
#define PP_P_PIXELS             8     // the node's params (PlantsNode::Params)
#define PP_P_MIN_ELEMENTS       9
#define PP_P_WOOD_RANGE         10
#define PP_P_RANGE              11
#define PP_P_SHADOW_TEXELS      12
#define PP_P_SHADOW_RANGE       13
#define PP_P_FRUSTUM            16    // the camera's six planes (normal xyz, d)
#define PP_P_TILE               40    // per tile, PP_TILE_WORDS
#define PP_TILE_WORDS           28
#define PP_TILE_FRUSTUM         0     // six planes
#define PP_TILE_TEXEL_CONST     24    // an ortho tile's texel (m)
#define PP_TILE_TEXEL_PER_METRE 25    // a perspective tile's, a metre from the eye
#define PP_TILE_ORTHO           26    // uint
#define PP_P_WORDS              (PP_P_TILE + PP_TILE_WORDS * PP_MAX_TILES)

// --- WORK: runs of PP_SLOTS ---
#define PP_N_LEAF               0                    // leaf chunks a slot (the count stage's)
#define PP_N_WOOD               (1 * PP_SLOTS)       // wood chunks a slot
#define PP_N_DISC               (2 * PP_SLOTS)       // crown discs a slot (a tile's)
#define PP_N_CURSOR             (3 * PP_SLOTS)       // x3: leaf, wood, disc cursors (the place stage's)
#define PP_N_LEAF_START         (6 * PP_SLOTS)       // a slot's first leaf chunk
#define PP_N_WOOD_START         (7 * PP_SLOTS)       // its first wood chunk
#define PP_N_DISC_START         (8 * PP_SLOTS)       // its first disc record
#define PP_N_DISC_CHUNK         (9 * PP_SLOTS)       // its first disc chunk (after its leaf chunks)
#define PP_N_DISC_FIT           (10 * PP_SLOTS)      // its discs that fit in CHUNKS
#define PP_N_STATS              (11 * PP_SLOTS)      // PP_S_WORDS, read back
#define PP_N_WORDS              (PP_N_STATS + PP_S_WORDS)
// The stats (PlantsNode::Stats, and the room asked for).
#define PP_S_PLANTS             0
#define PP_S_ELEMENTS           1
#define PP_S_LIMBS              2
#define PP_S_SHADOW_ELEMENTS    3
#define PP_S_RECORDS            4     // chunk records the frame asked for
#define PP_S_LEAF_CHUNKS        5     // leaf chunks (and disc chunks): the poses' slots asked
#define PP_S_WORDS              8

// --- ARGS ---
#define PP_A_POSE               0     // x, 1, 1: a workgroup a leaf chunk (plants_pose.comp)
#define PP_A_COMMANDS           4     // the draws' commands, PP_A_COMMAND_WORDS each:
#define PP_A_COMMAND_WORDS      8     //   2v a view's leaves (indexed), 2v + 1 its wood (view 0 the camera, 1 + t tile t)
#define PP_A_WORDS              (PP_A_COMMANDS + PP_A_COMMAND_WORDS * 2 * (1 + PP_MAX_TILES))

#endif
