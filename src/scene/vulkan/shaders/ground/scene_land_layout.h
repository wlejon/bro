// The land (scene/nodes/land_node.h): its draw's parameters and its fills'
// tiles, for C++ and GLSL alike. Every offset counts 32-bit words.
//
//   LandDraw (set 3, binding 0, std140): what mesh_land.vert reads,
//            LD_WORDS words
//   set 3, bindings 4..6: the tiles' heights, their surface, the page table
//
//   land_fill.comp's set 0: binding 0 the heights (r32f, an array layer a
//            slot), 1 the surface (rgba16f), 2 the fills (LF_TILE_WORDS a
//            tile), 3 the module's data (vec4s); push constants LF_P_*

#ifndef BRO_SCENE_LAND_LAYOUT_H
#define BRO_SCENE_LAND_LAYOUT_H

// --- LandDraw ---
#define LD_LAND             0     // x the land's extent (half-size, m), yz the frame's eye's world x and z
                                  // (the rings' centre in every pass, a probe's and a tile fill's too); w unused
#define LD_RINGS            4     // x the finest ring's cell (m), y 4 / resolution (the rings' K, mesh_land.vert),
                                  // z the rings, w resolution / 2
#define LD_TILES            8     // x a level-0 tile's side (m), y its texels inside its border, z the border,
                                  // w the tile levels
#define LD_PAGE             12    // x the page shift (log2 of the entries a side), y the entries a side,
                                  // z unused, w 1 when any tile is resident
#define LD_WORDS            16

// --- the fills ---
#define LF_GROUP            8     // a workgroup is LF_GROUP x LF_GROUP texels of one tile
#define LF_TILE_WORDS       4     // x0, z0 (the corner of its border: its own corner less the border), its texel (m), its slot
#define LF_P_STRIDE         0     // uint: a tile's texels a side with its border
#define LF_P_BORDER         1     // uint
#define LF_P_TILES          2     // uint: the fills this dispatch
#define LF_P_WORDS          4

#endif
