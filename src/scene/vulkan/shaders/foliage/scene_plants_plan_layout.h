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
#define PP_LEAF_POINTS          10    // points an element's polygon (PLANT_LEAF_POINTS)
#define PP_LEAF_INDICES         24    // indices an element, the camera's (the drawer's leafIndices)
#define PP_CASTER_INDICES       12    // a shadow view's: its coarse polygon (casterIndices)
// The camera's bands under PP_NEAR_BANDS draw an element's whole outline,
// the rest its coarse polygon (the casters' indices): band 16 starts at
// 2^4 - 1 = 15 m, and an outline's detail is 0 past PLANT_OUTLINE_FAR (10 m,
// scene_plants.glsl), a crown's reach inside that, so a plant crossing it
// changes nothing on screen.
#define PP_NEAR_BANDS           16
#define PP_TUBE_VERTS           36   // vertices a tube (PLANT_TUBE_VERTS)
#define PP_MAX_LIMBS            48    // a species' tubes besides its trunk (PLANT_MAX_LIMBS)
// A plant of at most PP_PACK_MOST elements (a far one) shares a leaf chunk:
// class c gives each plant 2^c of its slots, PP_LEAVES_PER_CHUNK >> c plants
// a chunk, so a tree at the horizon poses and draws one element, not a chunk.
#define PP_PACK_CLASSES         5
#define PP_PACK_MOST            16
#define PP_PACK_WORDS           (PP_SLOTS * PP_PACK_CLASSES)   // a slot's classes, slot-major

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
#define PP_P_STILL_PENUMBRA     14    // ShadowPlan::stillPenumbra
#define PP_P_PAINTED            15    // past the real band (PP_P_RANGE) to this (m), a plant painted (PlantsNode::paintReach)
#define PP_P_FRUSTUM            16    // the camera's six planes (normal xyz, d)
#define PP_P_TILE               40    // per tile, PP_TILE_WORDS
#define PP_TILE_WORDS           29
#define PP_TILE_FRUSTUM         0     // six planes
#define PP_TILE_TEXEL_CONST     24    // an ortho tile's texel (m)
#define PP_TILE_TEXEL_PER_METRE 25    // a perspective tile's, a metre from the eye
#define PP_TILE_ORTHO           26    // uint
#define PP_TILE_PLANTS          27    // uint: which crowns it takes (PP_PLANTS_*)
#define PP_TILE_PENUMBRA        28    // the sun's penumbra a metre above the receiver (m)
#define PP_PLANTS_ALL           0u
#define PP_PLANTS_SWAYING       1u    // those whose penumbra at the crown's middle is at most stillPenumbra
#define PP_PLANTS_STILL         2u    // the rest
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
#define PP_N_PACK               (11 * PP_SLOTS)                     // packed plants a slot's class
#define PP_N_PACK_CURSOR        (PP_N_PACK + PP_PACK_WORDS)         // the place stage's cursors
#define PP_N_PACK_START         (PP_N_PACK + 2 * PP_PACK_WORDS)     // a class's first packed record
#define PP_N_PACK_CHUNK         (PP_N_PACK + 3 * PP_PACK_WORDS)     // its first chunk (after the slot's leaf chunks)
#define PP_N_PACK_FIT           (PP_N_PACK + 4 * PP_PACK_WORDS)     // its records that fit in CHUNKS
#define PP_N_STATS              (PP_N_PACK + 5 * PP_PACK_WORDS)     // PP_S_WORDS, read back
// The camera's painted plants (one record each, the plant: plants.vert's
// painted crown), in two lists, the near (PP_PAINT_SIDES a fan) and the
// far (PP_PAINT_FAR_SIDES: a crown under PP_PAINT_FAR_PIXELS of radius on
// screen): each list's count, the place stage's cursor, its first record
// and those that fit in CHUNKS, list-major (PP_N_PAINT + 4 * list).
#define PP_N_PAINT              (PP_N_STATS + PP_S_WORDS)
#define PP_N_PAINT_CURSOR       (PP_N_PAINT + 1)
#define PP_N_PAINT_START        (PP_N_PAINT + 2)
#define PP_N_PAINT_FIT          (PP_N_PAINT + 3)
#define PP_N_WORDS              (PP_N_PAINT + 8)
// The stats (PlantsNode::Stats, and the room asked for).
#define PP_S_PLANTS             0
#define PP_S_ELEMENTS           1
#define PP_S_LIMBS              2
#define PP_S_SHADOW_ELEMENTS    3
#define PP_S_RECORDS            4     // chunk records the frame asked for
#define PP_S_LEAF_CHUNKS        5     // leaf chunks (and disc chunks): the poses' slots asked
#define PP_S_PAINTED            6     // plants painted (the camera's)
#define PP_S_WORDS              8

// --- ARGS ---
#define PP_A_POSE               0     // x, 1, 1: a workgroup a leaf chunk (plants_pose.comp)
#define PP_A_COMMANDS           4     // the draws' commands, PP_A_COMMAND_WORDS each:
#define PP_A_COMMAND_WORDS      8     //   2v a view's leaves (indexed), 2v + 1 its wood (view 0 the camera, 1 + t tile t)
#define PP_A_FAR_LEAVES         (2 * (1 + PP_MAX_TILES))   // then the camera's far bands' leaves (indexed, coarse)
#define PP_A_PAINT              (PP_A_FAR_LEAVES + 1)      // then its painted crowns, near then far (non-indexed)
#define PP_A_WORDS              (PP_A_COMMANDS + PP_A_COMMAND_WORDS * (PP_A_PAINT + 2))
// A painted crown: a fan of PP_PAINT_SIDES triangles about its middle
// (plants.vert), drawn as a list; one under PP_PAINT_FAR_PIXELS of radius
// on screen, of PP_PAINT_FAR_SIDES (its outline's lumps are under a pixel
// or two there).
#define PP_PAINT_SIDES          24
#define PP_PAINT_FAR_SIDES      6
#define PP_PAINT_FAR_PIXELS     6.0

#endif
