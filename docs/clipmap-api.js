// =============================================================================
// bro Clipmap Terrain API Reference
// =============================================================================
//
// A camera-centred GEOMETRY CLIPMAP: concentric square rings of fixed topology,
// built once, parked on the camera, displaced on the GPU from a streamed height
// pyramid. This is the "continuous world from underfoot to the horizon" case.
//
// How it differs from scene.createTerrain (docs/terrain-api.js), which is NOT
// replaced and stays the voxel/editable terrain:
//
//   createTerrain          createClipmapTerrain
//   ---------------------  ---------------------------------------------------
//   chunk meshes stream    ONE mesh, built once, never rebuilt
//   CPU heightmaps         GPU vertex displacement from an R32F mip pyramid
//   editable (setVoxel)    read-only surface; you edit the height TEXTURE
//   LOD rings pop/stitch   cracks impossible by construction (see below)
//   raycast + materials    elevationAt() + plain slope shading
//
// Why there are no cracks. Vertex displacement is a PURE FUNCTION of world XZ,
// sampled with textureLod() at a FRACTIONAL mip level that depends only on
// distance from the camera. Never on which ring a vertex belongs to. Two rings
// meeting at a boundary therefore evaluate the same function at the same
// position and land on the same height. Nothing is stitched, because nothing
// can disagree. The fractional mip also means GL's trilinear filtering blends
// LOD transitions instead of popping them.
//
// Why the triangle budget is flat. Ring n covers 4x the area at the same
// triangle count, so screen-space triangle density is roughly constant whether
// the camera is on the ground or in orbit. Nothing streams except texture data.
//
// The node is one MeshNode with a custom shader, so it inherits the normal 3D
// pipeline: PBR lighting, fog, shadow RECEIVING, frustum culling, post-FX.
//
//   const scene   = canvas.getContext('scene');
//   const clipmap = scene.createClipmapTerrain({ levels: 10, resolution: 128 });
// =============================================================================


// -----------------------------------------------------------------------------
// scene.createClipmapTerrain(opts) → ClipmapTerrain
// -----------------------------------------------------------------------------
//
// {
//   levels:           10,     // number of concentric rings (1..20). Ring l has cell
//                             // size cellSize * 2^l, so the stack reaches
//                             //   (resolution/2) * cellSize * 2^(levels-1) metres.
//   resolution:       128,    // quads per ring per axis. Rounded to a multiple of 4
//                             // (the central hole is inset by resolution/4). Drives
//                             // both triangle count and how far each ring reaches.
//   cellSize:         1.0,    // metres per cell at level 0, the finest detail the
//                             // geometry can express, right under the camera.
//   heightScale:      1.0,    // sampled texel value -> metres
//   seaLevel:         0.0,    // metres added to every sample
//   snowLine:         3500.0, // altitude in metres where snow blending begins
//   planetRadius:     0.0,    // planet radius for spherical curvature (0 = flat)
//   detailWavelength: 4.0,    // procedural micro-detail noise wavelength
//   detailRelief:     0.5,    // procedural micro-detail amplitude
//   detailGain:       0.5,    // micro-detail octave gain
//   detailOctaves:    4,      // micro-detail octave count
// }
//
// Total triangles ~= 2 * resolution^2 * (levels/4 + 3/4). With the defaults
// (128 / 10) that is roughly 250k, fixed, forever, regardless of view.
//
// The node is added to the scene root immediately and starts rendering flat at
// y = seaLevel until you install a height layer.
//
//   const clipmap = scene.createClipmapTerrain({
//     levels: 10, resolution: 128, cellSize: 30, heightScale: 1,
//   });


// -----------------------------------------------------------------------------
// clipmap.setHeightLayer(index, desc | null) → clipmap   (chainable)
// -----------------------------------------------------------------------------
//
// Install one level of the height pyramid. Up to 4 layers (index 0..3),
// ordered FINEST FIRST. Together they are the multi-scale height source; the
// intended shape is something like a 30 m decoder field over the player, a
// 240 m regional field, and a 7.68 km world field.
//
//   desc = {
//     data:          Float32Array,   // width*height samples, row-major
//     width:         Number,         // texels
//     height:        Number,
//     originX:       Number,         // WORLD metres of texel (0, 0)
//     originZ:       Number,
//     metresPerCell: Number,         // world metres between adjacent texels
//     wrapX:         Boolean,        // periodic in X (for planetary charts, default false)
//     bandLimited:   Boolean,        // the data really does reach its own Nyquist
//                                    // (default false — see below)
//   }
//
// Texel (i, j) therefore sits at world
//     x = originX + i * metresPerCell
//     z = originZ + j * metresPerCell
// Getting that wrong shifts a layer against the others, which looks entirely
// plausible and simply does not line up.
//
// `data` is copied and uploaded as an R32F texture WITH A MIP CHAIN. The chain
// is not an optimisation, the shader samples at a fractional lod, and without
// a chain GL clamps every lod to level 0 and the whole distance-continuous
// filtering story collapses.
//
// Blending. The COARSEST present layer is the base and is assumed to cover
// everything. Each finer layer is then blended in by a coverage weight that is
// 1 well inside its footprint and falls smoothly to 0 over the outer 8% (per
// axis) of its extent. So running off a fine layer's coverage degrades
// gradually into the layer beneath it instead of showing a seam. Outside every
// layer, GL_CLAMP_TO_EDGE holds the border value rather than folding to zero.
//
// bandLimited — WHO OWNS THE FINE END, the data or the procedural detail.
//
// The GPU synthesises detail below the data (see setDetail), and it has to know
// where "below" starts. A grid of cell d can represent no wavelength shorter
// than 2d, so the honest answer is 2 * metresPerCell — detail owns everything
// finer, the layer owns everything coarser, and the two never overlap.
//
// That is only true of data that actually reaches its own Nyquist. A SMOOTH
// LEARNED OR DECODED field does not: bro.worldgen's 30 m elevation carries a
// mountain's macro shape but none of its sub-kilometre ruggedness, so trusting
// it down to 60 m leaves the mountainside glassy — correctly lit, correctly
// shaped, and made of glass. For those, detail is deliberately allowed to
// overlap the band the decoder rendered flat, roughening it from a fixed
// kilometre-scale ceiling instead. That is the DEFAULT, inferred from a data
// floor finer than ~400 m, and it is what a streamed decoder layer wants.
//
// Set bandLimited: true when the layer is not that — a field you generated,
// eroded or authored at its own cell size, which genuinely carries content down
// to 2 * metresPerCell. The high-pass then sits at the layer's own Nyquist,
// where it belongs. Left false, such a layer gets roughened over a band it
// already occupies: four extra octaves for a 32 m layer, each adding another
// detailRelief to the shading tangent, which tips the normal past the
// terminator on ground already near it and speckles every steep slope. The
// symptom looks like a lighting or noise bug and is neither.
//
// Rules of thumb:
//
//   coarse world chart (km/cell)   either — the heuristic never fires that
//                                  coarse, so the flag changes nothing
//   streamed learned/decoded       leave false (roughening is the point)
//   procedural / eroded / authored bandLimited: true
//   real DEM resampled fine        true if it was band-limited on the way down,
//                                  false if it was upsampled from something
//                                  coarser and is smooth between samples
//
// It is per LAYER, not per terrain, so a stack can mix them: a smooth 30 m
// decoder window over a band-limited procedural regional field is a legitimate
// and correctly handled combination. Across a layer edge the two rules cross
// over on the same coverage ramp the heights blend on.
//
// It also settles a divergence: elevationAt() high-passes at the layer's own
// Nyquist unconditionally, so on a band-limited layer the query and the drawn
// surface agree, while on a roughened one the GPU adds coarse octaves the query
// does not carry.
//
// Passing null (or omitting the descriptor) releases the layer's pixels.
//
//   clipmap
//     .setHeightLayer(2, { data: world,    width: 512, height: 512,
//                          originX: -1966080, originZ: -1966080,
//                          metresPerCell: 7680 })
//     .setHeightLayer(1, { data: regional, width: 512, height: 512,
//                          originX: -61440, originZ: -61440,
//                          metresPerCell: 240 })
//     .setHeightLayer(0, { data: local,    width: 512, height: 512,
//                          originX: px - 7680, originZ: pz - 7680,
//                          metresPerCell: 30 });
//
//   clipmap.setHeightLayer(0, null);   // release the fine field


// -----------------------------------------------------------------------------
// Additional styling and layer configuration methods
// -----------------------------------------------------------------------------
//
// clipmap.setSnowLine(altitudeMetres) → clipmap
//   Set the altitude in metres where snow blending begins.
//
// clipmap.setDetail({ wavelength, relief, gain, octaves }) → clipmap
//   Configure procedural micro-detail noise applied on the GPU shader.
//
// clipmap.setMaterials({ rock, snow, sand, grass }) → clipmap
//   Set material PBR properties ({ albedo: [r,g,b], roughness: number }) per biome.
//
// clipmap.setForest({ albedo: [r,g,b], strength: 0..1 }) → clipmap
//   Set L0 forest canopy tint color and blend strength.
//
// clipmap.setSurfaceLayer({ data, width, height, originX, originZ, metresPerCell }) → clipmap
//   Install a 3-channel RGB surface map (width*height*3 Float32Array).
//
// clipmap.coverageDistance(eyeAboveSeaLevel) → Number
//   Radius in metres across which height data is required for a camera at eyeAboveSeaLevel.
//
// clipmap.horizonDistance(eyeAboveSeaLevel) → Number
//   Distance to the curved planet horizon in metres (Infinity on flat worlds).


// -----------------------------------------------------------------------------
// clipmap.update(camX, camY, camZ) → clipmap   (chainable)
// -----------------------------------------------------------------------------
//
// Call EVERY FRAME with the camera eye. This does three things:
//   - parks the node's world position on the eye, which is what leaves the
//     model matrix at identity for the camera-relative vertex shader (the whole
//     matrix pipeline is fp32, keeping object space small is what makes a
//     100 km world stable);
//   - pushes the camera uniforms the rings snap and filter against;
//   - refreshes the cull margin, which depends on camera altitude.
//
// It does NOT rebuild geometry, allocate, or touch the CPU height data. It is
// a handful of uniform writes. There is no load budget to throttle and no
// equivalent of terrain.rebuild().
//
//   function frame() {
//     clipmap.update(cam.x, cam.y, cam.z);
//     ...
//   }


// -----------------------------------------------------------------------------
// clipmap.elevationAt(x, z) → Number
// -----------------------------------------------------------------------------
//
// World-metre ground height at a world XZ. This is the collision/gameplay
// query: same layer selection, same coverage blending and the same
// heightScale/seaLevel the GPU applies, so a character placed with it stands on
// the visible surface rather than through it.
//
// One documented divergence: the CPU path samples the BASE level (bilinear at
// mip 0) of each layer, while the GPU moves to a coarser mip with distance. So
// elevationAt is EXACT near the camera, the region that matters for collision,
// where the GPU is also on level 0, and APPROXIMATE far away, where the GPU is
// rendering a smoothed version of the same field. Do not use it to place
// objects tens of kilometres away and expect them to sit exactly on the pixels.
//
//   const y = clipmap.elevationAt(player.x, player.z);
//   player.y = y + 1.8;


// -----------------------------------------------------------------------------
// clipmap.node → SceneNode
// -----------------------------------------------------------------------------
//
// The MeshNode carrying the ring geometry. Null after destroy(). Use it for
// anything the mesh surface exposes, visibility, material tweaks, render
// order:
//
//   clipmap.node.visible = false;
//
// Two defaults worth knowing:
//
//   castsShadow = false. A ring stack tens of kilometres across would swallow
//   the whole shadow-atlas fit if it were a caster, leaving every other caster
//   with a few texels. It still RECEIVES shadows, which is what matters for
//   objects standing on it. Set clipmap.node.castsShadow = true only if you
//   have also constrained your shadow rig.
//
//   cullMargin is managed per update(), not fixed. Frustum culling cannot see
//   GLSL displacement, and this node's baked AABB is a flat sheet at y = 0 in
//   an object space parked at the eye, every vertex the shader emits is
//   outside it. The margin is recomputed each update from (a) the sample
//   extremes across the installed layers relative to the current camera
//   altitude and (b) the per-level grid-snap slop. Culling is deliberately left
//   ON rather than disabled: this is one of the largest nodes in any scene that
//   has it, and it should still drop out when the camera looks away from it.
//   Do not overwrite clipmap.node.cullMargin, update() will replace it.


// -----------------------------------------------------------------------------
// clipmap.destroy()
// -----------------------------------------------------------------------------
//
// Destroy the node and release every layer. Idempotent; every accessor stays
// safe afterwards (node reads null, elevationAt returns 0). Terrains are also
// torn down automatically at app reload and engine shutdown.


// -----------------------------------------------------------------------------
// Read-only properties
// -----------------------------------------------------------------------------
//
//   clipmap.levels          Number   ring count (after clamping)
//   clipmap.resolution      Number   quads per ring per axis (after rounding)
//   clipmap.cellSize        Number   metres per cell at level 0
//   clipmap.layerCount      Number   highest installed layer index + 1
//   clipmap.triangleCount   Number   fixed for the node's lifetime
//   clipmap.vertexCount     Number   likewise
//   clipmap.farDistance     Number   outer half-extent of the coarsest ring,
//                                    in metres, past this there is no geometry
//   clipmap.snowLine        Number   altitude in metres where snow begins
//   clipmap.planetRadius    Number   curvature radius in metres
//   clipmap.seaLevel        Number   base altitude offset


// -----------------------------------------------------------------------------
// Full example: streamed world + player collision
// -----------------------------------------------------------------------------
//
//   const canvas  = document.querySelector('canvas');
//   const scene   = canvas.getContext('scene');
//   scene.createLight({ type: 'directional', direction: [0.3, -0.9, 0.2],
//                       intensity: 3 });
//
//   const clipmap = scene.createClipmapTerrain({
//     levels: 10, resolution: 128, cellSize: 4, heightScale: 1, seaLevel: 0,
//   });
//
//   // Coarse world field first. It is the base of the blend and must cover
//   // everywhere the camera can reach.
//   clipmap.setHeightLayer(1, {
//     data: worldHeights, width: 512, height: 512,
//     originX: -1966080, originZ: -1966080, metresPerCell: 7680,
//   });
//
//   const player = { x: 0, z: 0, y: 0 };
//   let lastTileX = null, lastTileZ = null;
//
//   function frame() {
//     // Restream the fine field when the player crosses into a new tile. Only
//     // TEXTURE data moves; the geometry never changes.
//     const tx = Math.floor(player.x / 7680), tz = Math.floor(player.z / 7680);
//     if (tx !== lastTileX || tz !== lastTileZ) {
//       lastTileX = tx; lastTileZ = tz;
//       clipmap.setHeightLayer(0, {
//         data: decodeLocalField(tx, tz), width: 512, height: 512,
//         originX: tx * 7680 - 7680, originZ: tz * 7680 - 7680,
//         metresPerCell: 30,
//       });
//     }
//
//     player.y = clipmap.elevationAt(player.x, player.z) + 1.7;
//     scene.setCamera({ fov: 60, near: 1, far: 200000,
//                       position: [player.x, player.y, player.z],
//                       target: [player.x + dirX, player.y, player.z + dirZ] });
//     clipmap.update(player.x, player.y, player.z);
//     requestAnimationFrame(frame);
//   }
//   requestAnimationFrame(frame);


// -----------------------------------------------------------------------------
// Limitations
// -----------------------------------------------------------------------------
//
//  - Height field only. No overhangs, caves or arches, and no editing API,
//    change the terrain by re-uploading a height layer.
//  - Finite reach. Beyond farDistance there is no geometry; size `levels` so
//    the stack covers the highest camera you allow.
//  - At most 4 layers (the mesh user-sampler budget is 6 units on GL 3.3, and
//    a sampler array cannot be dynamically indexed there, so the shader
//    unrolls a fixed 4).
//  - scene.raycast() does not hit it usefully: the collision surface lives in
//    the vertex shader, not in the mesh's BVH, which sees a flat sheet at
//    y = 0. Use elevationAt().
//  - Shading is deliberately plain (neutral base colour, slope-driven
//    variation). Materials, splatting and procedural detail are not here yet.
