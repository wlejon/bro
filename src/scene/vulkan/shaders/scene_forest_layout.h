// The forest node's GPU layout, for C++ and GLSL alike: ForestNode::gpuData
// writes the node's data by these names, the forest drawer its draw entries,
// and the forest shaders read both by them (scene_forest.glsl's FOREST_*
// accessors). Plain #defines, so the one file is a C++ header
// (scene/forest_node.cpp, scene/vulkan/scene_mesh_drawer_forest.cpp) and a
// GLSL include (scene_procedural.glsl).
//
// Every offset counts floats from the start of its record run; a run is
// whole vec4 records (FOREST_*_RECORDS of them), so a float's record is its
// offset / 4 and its lane its offset % 4. A field marked "aligned" starts a
// record (its xyz or xyzw is that record's). "bits": a uint held as float
// bits (floatBitsToUint).
//
// Where a run copies one of broflora's records verbatim (a segment, a
// clump, a vegetation record, a bloom), its offsets are broflora's
// (broflora/forest_records.h) from the copy's first float: ForestNode
// static_asserts the two agree.

#ifndef BRO_SCENE_FOREST_LAYOUT_H
#define BRO_SCENE_FOREST_LAYOUT_H

// --- The header: 16 floats (set 3 binding 1's header[4]) ---
#define FOREST_H_SEED           0    // bits: the leaves' placement seed (the scatter's)
#define FOREST_H_UP_BIAS        1
#define FOREST_H_TILT_JITTER    2
#define FOREST_H_SHELL          3
#define FOREST_H_BASE_SCALE     4
#define FOREST_H_SCALE_JITTER   5
#define FOREST_H_LEAF_LENGTH    6
#define FOREST_H_FLUTTER_FADE   7    // the widening the leaves' flutter fades out by
#define FOREST_H_SPECIMENS      8    // bits: first record of each run (the four aligned)
#define FOREST_H_SEGMENTS       9
#define FOREST_H_CLUMPS         10
#define FOREST_H_LEAVES         11
#define FOREST_H_INSTANCES      12   // bits
#define FOREST_H_LEAF_ASPECT    13   // the leaf's width over its length (a spray's: its needles' cover)
#define FOREST_H_FLUTTER        14
#define FOREST_H_LOOK           15   // bits: leaf form (bits 0-3), limb bob, twig bob (64ths, 8 bits each)
#define FOREST_HEADER_FLOATS    16

// --- A specimen: 9 records ---
#define FOREST_SPECIMEN_RECORDS 9
#define FOREST_SP_SEG_START     0    // bits (the first four aligned: one uvec4)
#define FOREST_SP_SEG_COUNT     1    // bits
#define FOREST_SP_CLUMP_START   2    // bits
#define FOREST_SP_CLUMP_COUNT   3    // bits
#define FOREST_SP_LEAF_START    4    // bits
#define FOREST_SP_LEAF_COUNT    5    // bits
#define FOREST_SP_CROWN_CLUMP   6    // bits: the clump nearest the crown's middle
#define FOREST_SP_CLUMP_WEIGHT  7    // its clumps' mean branch weight
#define FOREST_SP_CROWN         8    // aligned: the crown's centre (xyz)
#define FOREST_SP_CROWN_RADIUS  11   // across
#define FOREST_SP_CROWN_UP      12   // the crown's radius up
#define FOREST_SP_CLUMP_RADIUS  13   // its clumps' mean radius
#define FOREST_SP_HEIGHT        14
#define FOREST_SP_BLOCKS        15   // bits: its first clump block's record
#define FOREST_SP_LEAF_TINT     16   // aligned: its leaves' colour over the node's (rgb)
#define FOREST_SP_CLUMP_FLAT    19   // its clumps' height over their width
#define FOREST_SP_BARK_TINT     20   // aligned: its bark's colour over the node's (rgb)
#define FOREST_SP_GLOSS         23   // its leaves' gloss
#define FOREST_SP_BLOOM_COLOR   24   // aligned: its blooms' colour (rgb, linear, absolute)
#define FOREST_SP_BLOOM_FORM    27   // bits: 0 none, 1 flower, 2 head, 3 spike
#define FOREST_SP_LEAF          28   // aligned: its broad leaf: width over length, tip, teeth a side, tooth depth
#define FOREST_SP_BRUSH         32   // aligned: its sprays' brush,
#define FOREST_SP_PETIOLE       33   //   its broad leaf's petiole (of the blade's length),
#define FOREST_SP_PHYLLOTAXIS   34   //   and phyllotaxis (0 alternate, 1 opposite, 2 spiral, 3 pinnate)
#define FOREST_SP_FREE          35   // unused (0)

// --- A segment: 8 records (broflora's segment, its tangents, its vegetation, its clumps) ---
#define FOREST_SEGMENT_RECORDS  8
#define FOREST_SEG_FROM         0    // aligned: from.xyz, rFrom
#define FOREST_SEG_R_FROM       3
#define FOREST_SEG_TO           4    // aligned: to.xyz, rTo (negative: a tip, closing over)
#define FOREST_SEG_R_TO         7
#define FOREST_SEG_TANGENTS     8    // aligned: the octahedral tangent at from (xy), at to (zw)
#define FOREST_SEG_VEGETATION   12   // aligned: its vegetation record (4 records)
#define FOREST_SEG_CLUMPS       28   // aligned, bits: the few clumps it leads to (global; broflora's
                                     //   segment_clumps), FOREST_SEG_NO_CLUMP past the last
#define FOREST_SEG_CLUMP_SLOTS  4
#define FOREST_SEG_NO_CLUMP     0xffffffffu

// --- A clump: 7 records (broflora's clump, its vegetation, its drop group) ---
#define FOREST_CLUMP_RECORDS    7
#define FOREST_CLUMP_CENTRE     0    // aligned: centre.xyz, radius
#define FOREST_CLUMP_RADIUS     3
#define FOREST_CLUMP_TWIG       4    // aligned: its twig's direction.xyz, its leaf count
#define FOREST_CLUMP_LEAVES     7
#define FOREST_CLUMP_VEGETATION 8    // aligned: its vegetation record (4 records)
// The vegetation record's path (which no clump stage reads) carries the
// clump's first placement key (bits: scene_forest_shoot.glsl).
#define FOREST_CLUMP_KEY_BASE   (FOREST_CLUMP_VEGETATION + FOREST_VEG_PATH)
#define FOREST_CLUMP_GROUP      24   // aligned, bits: its drop group's ordinal in its specimen (broflora's
                                     //   clumpGroups, in clump order): a tree leaves the group bare or not as one

// --- A vegetation record within a run: 4 records (broflora's 16 floats) ---
#define FOREST_VEG_ROOT         0    // aligned: root.xyz, height
#define FOREST_VEG_HEIGHT       3
#define FOREST_VEG_CROWN        4    // aligned: crown.xyz, crown radius
#define FOREST_VEG_CROWN_RADIUS 7
#define FOREST_VEG_WEIGHT_FROM  8    // aligned: weightFrom, weightTo, path, bark word
#define FOREST_VEG_WEIGHT_TO    9
#define FOREST_VEG_PATH         10
#define FOREST_VEG_BARK         11
#define FOREST_VEG_AXIS         12   // aligned: axis (0 here), twig, limb, trunk words
#define FOREST_VEG_TWIG         13
#define FOREST_VEG_LIMB         14
#define FOREST_VEG_TRUNK        15

// --- A clump block: 4 records per run of FOREST_CLUMP_BLOCK of a specimen's clumps ---
#define FOREST_CLUMP_BLOCK      8
#define FOREST_BLOCK_RECORDS    4
#define FOREST_BLOCK_SPHERE     0    // aligned: the sphere round their ellipsoids (centre.xyz, radius)
#define FOREST_BLOCK_CLUMPS     4    // aligned: bits: the clumps, 16 bits each, two a float, 0xffff past the last
#define FOREST_BLOCK_FILL       8    // aligned: the upright ellipsoid the run fills (centre.xyz, radius across)
#define FOREST_BLOCK_FILL_UP    12   // its radius up
#define FOREST_BLOCK_LEAVES     13   // the run's leaves
// --- A run group: before a specimen's first run (FOREST_SP_BLOCKS), 2 records
// for each FOREST_BLOCK_GROUP of its runs, in order ---
#define FOREST_BLOCK_GROUP      8
#define FOREST_GROUP_RECORDS    2
#define FOREST_GROUP_FILL       0    // aligned: the upright ellipsoid its runs' clumps fill (centre.xyz, radius across)
#define FOREST_GROUP_FILL_UP    4    // its radius up
#define FOREST_GROUP_LEAVES     5    // the group's leaves

// --- Leaves: two a record (clump, placement key), bits ---
#define FOREST_LEAVES_PER_RECORD 2

// --- An instance: 2 records ---
#define FOREST_INSTANCE_RECORDS 2
#define FOREST_INST_POSITION    0    // aligned: position.xyz, scale
#define FOREST_INST_SCALE       3
#define FOREST_INST_COS_YAW     4    // aligned
#define FOREST_INST_SIN_YAW     5
#define FOREST_INST_SPECIMEN    6    // bits
#define FOREST_INST_SEED        7

// --- A bloom: 2 records (broflora's bloom, its segment made global) ---
#define FOREST_BLOOM_RECORDS    2
#define FOREST_BLOOM_POSITION   0    // aligned: position.xyz (its twig's tip), size
#define FOREST_BLOOM_SIZE       3
#define FOREST_BLOOM_DIR        4    // aligned: its twig's outward direction.xyz
#define FOREST_BLOOM_SEGMENT    7    // bits: its twig's segment (global)

// --- A draw entry (set 3 binding 2): 3 records ---
#define FOREST_ENTRY_RECORDS    3
#define FOREST_E_INSTANCE       0    // aligned, bits: instance, first, end, param
#define FOREST_E_FIRST          1
#define FOREST_E_END            2
#define FOREST_E_PARAM          3
#define FOREST_E_WIDENING       4    // aligned: widening, keep end, tail, fade
#define FOREST_E_KEEP_END       5
#define FOREST_E_TAIL           6
#define FOREST_E_FADE           7
#define FOREST_E_SLOT           8    // aligned, bits: its first leaf's pose (-1: none)
#define FOREST_E_WOOD           9    // the tint toward the bark (rgb)

// --- The draws: what the drawer sizes them by and the vertex stages number by ---
// A card is two triangles, unindexed (a bloom's, a crown's, each lobe of a
// clump's sprite: a clump's three lobes, FOREST_CLUMP_VERTICES a clump).
#define FOREST_CARD_VERTICES    6
#define FOREST_CLUMP_LOBES      3
#define FOREST_CLUMP_VERTICES   (FOREST_CLUMP_LOBES * FOREST_CARD_VERTICES)
// A sprite draw's kind (its push extra.z): a clump's lobes, a crown, a
// needle or scale lobe's core, or (a caster only) a run of clumps as the
// ellipsoid it fills (its block's FOREST_BLOCK_FILL), an entry's first and
// end its runs.
#define FOREST_SPRITE_CLUMPS    0
#define FOREST_SPRITE_CROWNS    1
#define FOREST_SPRITE_CORES     2
#define FOREST_SPRITE_RUNS      3
// Bark's sides (forestTubeMorph morphs a ring between them); a bark entry's
// param is its sides | its rows << FOREST_BARK_ROWS_SHIFT.
#define FOREST_BARK_SIDES_FAR       5
#define FOREST_BARK_SIDES_NEAR      10
#define FOREST_BARK_SIDES_NEAREST   20
#define FOREST_BARK_ROWS_SHIFT      8
#define FOREST_BARK_SIDES_MASK      255
// A twig draw's push extra.z: this plus its rows a piece.
#define FOREST_TWIG_ROWS_BIAS   2
// A posed leaf: 4 uvec4s (set 3 binding 3; scene_forest.glsl's ForestPose).
#define FOREST_POSE_RECORDS     4
// A shoot's twig radius at its base, in leaf lengths (forestPipe's unit).
#define FOREST_SHOOT_RADIUS_BASE 0.011
// What one draw entry spans (the drawer's addTree, on the CPU and in the GPU
// planner, scene_forest_plan.glsl): bark segments, clumps (a sprite's or a
// core's), a caster's runs of clumps, blooms, twig pieces (each a ribbon of
// FOREST_TWIG_VERTICES indices a row; a fine twig FOREST_TWIG_FINE_ROWS rows),
// and a far leaf card's or cluster's copies of the leaf (ForestNode::kCardCopies).
#define FOREST_SEGMENTS_PER_ENTRY    16
#define FOREST_CLUMPS_PER_ENTRY      16
#define FOREST_RUNS_PER_ENTRY        16
#define FOREST_BLOOMS_PER_ENTRY      16
#define FOREST_TWIG_PIECES_PER_ENTRY 64
#define FOREST_TWIG_VERTICES         6
#define FOREST_TWIG_FINE_ROWS        3
#define FOREST_CARD_COPIES           160

// What the readers take as given: an aligned field starts a record, and a
// record read whole has its fields in the lanes they are read from (its
// xyz and w, a header record's lanes).
#if FOREST_H_SPECIMENS % 4 != 0 || FOREST_H_SEGMENTS != FOREST_H_SPECIMENS + 1 || \
    FOREST_H_CLUMPS != FOREST_H_SPECIMENS + 2 || FOREST_H_LEAVES != FOREST_H_SPECIMENS + 3 || \
    FOREST_H_BASE_SCALE / 4 != FOREST_H_SCALE_JITTER / 4
#error "scene_forest_layout.h: the header's runs or its scale record moved"
#endif
#if FOREST_SP_SEG_START % 4 != 0 || FOREST_SP_CLUMP_START != FOREST_SP_SEG_START + 2 || \
    FOREST_SP_CLUMP_COUNT != FOREST_SP_SEG_START + 3 || FOREST_SP_LEAF_START % 4 != 0 || \
    FOREST_SP_LEAF_COUNT != FOREST_SP_LEAF_START + 1 || FOREST_SP_CROWN_CLUMP != FOREST_SP_LEAF_START + 2 || \
    FOREST_SP_CROWN % 4 != 0 || FOREST_SP_CROWN_RADIUS != FOREST_SP_CROWN + 3 || \
    FOREST_SP_LEAF_TINT % 4 != 0 || FOREST_SP_BARK_TINT % 4 != 0 || FOREST_SP_BLOOM_COLOR % 4 != 0 || \
    FOREST_SP_BLOOM_FORM != FOREST_SP_BLOOM_COLOR + 3 || FOREST_SP_LEAF % 4 != 0 || \
    FOREST_SP_FREE >= FOREST_SPECIMEN_RECORDS * 4
#error "scene_forest_layout.h: a specimen record's fields moved"
#endif
#if FOREST_SEG_FROM % 4 != 0 || FOREST_SEG_R_FROM != FOREST_SEG_FROM + 3 || FOREST_SEG_TO % 4 != 0 || \
    FOREST_SEG_R_TO != FOREST_SEG_TO + 3 || FOREST_SEG_TANGENTS % 4 != 0 || FOREST_SEG_VEGETATION % 4 != 0 || \
    FOREST_SEG_VEGETATION + 16 > FOREST_SEG_CLUMPS || FOREST_SEG_CLUMPS % 4 != 0 || \
    FOREST_SEG_CLUMPS + FOREST_SEG_CLUMP_SLOTS > FOREST_SEGMENT_RECORDS * 4
#error "scene_forest_layout.h: a segment record's fields moved"
#endif
#if FOREST_CLUMP_CENTRE % 4 != 0 || FOREST_CLUMP_RADIUS != FOREST_CLUMP_CENTRE + 3 || FOREST_CLUMP_TWIG % 4 != 0 || \
    FOREST_CLUMP_LEAVES != FOREST_CLUMP_TWIG + 3 || FOREST_CLUMP_VEGETATION % 4 != 0 || \
    FOREST_CLUMP_VEGETATION + 16 > FOREST_CLUMP_GROUP || FOREST_CLUMP_GROUP % 4 != 0 || \
    FOREST_CLUMP_GROUP >= FOREST_CLUMP_RECORDS * 4
#error "scene_forest_layout.h: a clump record's fields moved"
#endif
#if FOREST_VEG_ROOT != 0 || FOREST_VEG_HEIGHT != 3 || FOREST_VEG_CROWN != 4 || FOREST_VEG_CROWN_RADIUS != 7 || \
    FOREST_VEG_WEIGHT_FROM != 8 || FOREST_VEG_WEIGHT_TO != 9 || FOREST_VEG_PATH != 10 || FOREST_VEG_BARK != 11 || \
    FOREST_VEG_AXIS != 12
#error "scene_forest_layout.h: the vegetation record is scene_vegetation.glsl's v0..v3"
#endif
#if FOREST_INST_POSITION % 4 != 0 || FOREST_INST_SCALE != FOREST_INST_POSITION + 3 || FOREST_INST_COS_YAW % 4 != 0 || \
    FOREST_INST_SIN_YAW != FOREST_INST_COS_YAW + 1 || FOREST_BLOOM_POSITION % 4 != 0 || \
    FOREST_BLOOM_SIZE != FOREST_BLOOM_POSITION + 3 || FOREST_BLOOM_DIR % 4 != 0 || \
    FOREST_BLOCK_SPHERE % 4 != 0 || FOREST_BLOCK_CLUMPS % 4 != 0 || FOREST_BLOCK_FILL % 4 != 0
#error "scene_forest_layout.h: an instance, bloom or block record's fields moved"
#endif
#if FOREST_E_INSTANCE % 4 != 0 || FOREST_E_WIDENING % 4 != 0 || FOREST_E_SLOT % 4 != 0
#error "scene_forest_layout.h: a draw entry's records moved"
#endif
#if FOREST_BARK_SIDES_NEAREST > FOREST_BARK_SIDES_MASK || FOREST_BARK_SIDES_MASK + 1 != (1 << FOREST_BARK_ROWS_SHIFT) || \
    FOREST_BARK_SIDES_NEAR % FOREST_BARK_SIDES_FAR != 0 || FOREST_BARK_SIDES_NEAREST % FOREST_BARK_SIDES_NEAR != 0
#error "scene_forest_layout.h: a bark entry's sides overflow its param, or a level does not divide the next"
#endif

#endif
