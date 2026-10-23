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
//            canopy (rgba16f: its leaves' albedo, its cover), 6 the
//            processes' grid (rgba32f, read; scene_land_process.h); push
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
#define LG_CANOPY           11

// --- LandDraw ---
// The land's MAP (world/place.h): a flat body's x and z; on a round body
// the azimuthal map about the place's anchor, whose (x, z) is a point that
// far round the body from the anchor in that direction. The rings are laid
// out on it about the eye's map point. Its CHART is what the tiles are keyed
// on: a flat body's map, or a round body's six cube faces, each in face
// metres (place.h's chartOf); a face's LD_FACE words give its axes in the
// place frame and where the anchor lies on it, so a map point's chart is the
// anchor's plus a difference that keeps its precision near the eye
// (scene_land.glsl's landPosOf).
#define LD_LAND             0     // x how far the map reaches (m; the extent a flat land's half-size, a round
                                  // body's πR), yz the frame's eye's map x and z (the rings' centre in every
                                  // pass, a probe's and a tile fill's too), w unused
#define LD_RINGS            4     // x the finest ring's cell (m), y 4 / resolution (the rings' K, mesh_land.vert),
                                  // z the rings, w resolution / 2
#define LD_TILES            8     // x a level-0 tile's side (m), y its texels inside its border, z the border,
                                  // w the tile levels
#define LD_PAGE             12    // x the page shift (log2 of the entries a side), y the entries a side,
                                  // z the page table's layers a root (its levels' most), w 1 when any tile is
                                  // resident
#define LD_CANOPY           16    // the plants standing on it: x how far from the eye they are drawn (m),
                                  // y how far they cast, z 1 when the land carries their canopy, w unused
#define LD_BODY             20    // x the body's radius (m; 0 flat), y the ring ring 0 is drawn as (LandNode::
                                  // firstRing), zw unused
#define LD_EYE              24    // xyz the frame's eye in the land's frame (SceneView::world), w unused
#define LD_FACE             28    // a round body's six cube faces, LD_FACE_WORDS each: its n, u and v in the
                                  // place frame (xyz), then the anchor's chart on it (x, z) in level-0 tiles,
                                  // whole (xy) and fraction (zw)
#define LD_FACE_WORDS       16
#define LD_SEA              124   // its sea (LandNode::Sea): x 1 when it has one, y its level (m over the
                                  // datum), z its roughness, w unused; LD_SEA + 4 xyz its absorption (1/m),
                                  // LD_SEA + 8 xyz the colour deep water scatters back
#define LD_WORDS            136

// --- the fills ---
#define LF_GROUP            8     // a workgroup is LF_GROUP x LF_GROUP texels of one tile
#define LF_TILE_WORDS       52    // vec4 0: x0, z0 (the corner of its border: its own corner less the border;
                                  // on a round body from the tile's middle), its texel (m), its slot.
                                  // A round body's (land_fill.comp): vec4 1: the tile middle's tan(x / R),
                                  // tan(z / R), cos(x / R), cos(z / R); 2: its face's n (body frame), R;
                                  // 3: u, its root; 4: v; 5: the middle's datum point R n_c (body frame),
                                  // x and y as double-floats; 6: its z (xy), the middle's chart point (zw);
                                  // 7 + k: the datum point's high parts' chart point on the grid's face k
                                  // from its corner (chart + half), x and z high (xy) and low (zw)
#define LF_P_STRIDE         0     // uint: a tile's texels a side with its border
#define LF_P_BORDER         1     // uint
#define LF_P_TILES          2     // uint: the fills this dispatch
#define LF_P_WORDS          4

#endif
