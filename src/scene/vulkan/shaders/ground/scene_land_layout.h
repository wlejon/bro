// The land (scene/nodes/land_node.h): its draw's parameters and its fills'
// tiles, for C++ and GLSL alike. Every offset counts 32-bit words.
//
//   LandDraw (set 3, binding 0, std140): what mesh_land.vert reads,
//            LD_WORDS words
//   set 3, bindings 4..7: the tiles' heights, their surface, the page table,
//            the canopy
//
//   land_fill.comp's set 0: binding 0 the heights (rg32f, an array layer a
//            slot: the ground's, the canopy's top over it), 1 the surface
//            (rgba16f: albedo, herbs), 2 the fills (LF_TILE_WORDS a tile),
//            3 the module's data (vec4s), 4 the cover (rgba16f), 5 the
//            canopy (rgba16f: its leaves' albedo, its cover); push
//            constants LF_P_*
//
//   a grass node standing on the land (scene_grass.glsl) reads its tiles
//            through its own set 4: its GrassUBO carries the LandDraw
//            words, and the bindings are LG_*

#ifndef BRO_SCENE_LAND_LAYOUT_H
#define BRO_SCENE_LAND_LAYOUT_H

// --- a grass node's set 4 (scene_mesh_drawer_grass.cpp's writeSet) ---
#define LG_HEIGHTS          1
#define LG_COVERS           3
#define LG_SURFACES         9
#define LG_PAGE             10

// --- LandDraw ---
#define LD_LAND             0     // x the land's extent (half-size, m), yz the frame's eye's world x and z
                                  // (the rings' centre in every pass, a probe's and a tile fill's too); w its y
#define LD_RINGS            4     // x the finest ring's cell (m), y 4 / resolution (the rings' K, mesh_land.vert),
                                  // z the rings, w resolution / 2
#define LD_TILES            8     // x a level-0 tile's side (m), y its texels inside its border, z the border,
                                  // w the tile levels
#define LD_PAGE             12    // x the page shift (log2 of the entries a side), y the entries a side,
                                  // z unused, w 1 when any tile is resident
#define LD_CANOPY           16    // the plants standing on it: x how far from the eye they are drawn (m),
                                  // y how far they cast, z 1 when the land carries their canopy, w unused
#define LD_WORDS            20

// --- the fills ---
#define LF_GROUP            8     // a workgroup is LF_GROUP x LF_GROUP texels of one tile
#define LF_TILE_WORDS       4     // x0, z0 (the corner of its border: its own corner less the border), its texel (m), its slot
#define LF_P_STRIDE         0     // uint: a tile's texels a side with its border
#define LF_P_BORDER         1     // uint
#define LF_P_TILES          2     // uint: the fills this dispatch
#define LF_P_WORDS          4

#endif
