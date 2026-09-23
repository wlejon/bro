// =============================================================================
// bro.ai.game, Game AI API Reference
// =============================================================================
//
// The game AI API provides server-side pathfinding, steering, and perception
// for building game bots. Backed by the brogameagent C++ library.
//
// Available in all modes (windowed, headless, server), and inside a Worker:
// each realm installs its own copy of the classes.
//
// This file covers navigation. The rest of the namespace lives beside it:
//   docs/ai-game-planning.js  Agent, World, Unit, steering, perception, bindings
//   docs/ai-game-learning.js  MCTS family, planners, belief, simulation, replay
//   docs/ai-nn-api.js         bro.ai.game.nn: circuits, nets, ops, WeightsHandle
//   docs/ai-learn-api.js      bro.ai.game.learn: buffers, trainers, inference
//   docs/ai-game-tools.js     bro.ai.game.grid: obs windows, tapes, GridTrainer
//
// `globalThis.AI` is an alias for `bro.ai.game`, and every class is also a
// global constructor for `instanceof` checks: AINavGrid, AIHexNav, AINavMesh,
// AIAgent, AIWorld, AIUnit, AIAgentBinding, and the search/learn classes.
// The constructors are not callable; use the create*/bake* factories.
//
// Reserved properties. Handles store the JS callbacks and objects they
// depend on as own `_`-prefixed properties, so the collector can see them
// and a handle whose callbacks refer back to it can still be collected:
// `_callbacks` (search/planner callbacks), `_agents` and `_abilities`
// (World), `_world` and `_policies` (simulation), `_snapshots` and `_backend`
// (GenericMcts), and `_agent` (unit proxy, AgentBinding). They are internal:
// don't read, replace or delete them.
//
// A native exception inside any method (e.g. bad_alloc on a corrupt replay)
// throws a JS Error instead of ending the process.
//
// Integer options and arguments are checked across bro.ai.game (and its nn /
// learn / grid namespaces). Ids, teamId, counts, dims, iterations, budgetMs
// and the like are truncated toward zero like any JS integer conversion, but
// NaN, +-Infinity or a value outside the option's range is a RangeError
// naming the key ("<what> must be an integer in [lo, hi], got ...") instead
// of a wrapped value (a negative maxNeighbors once reached a
// resize((size_t)-1)). Absent keys keep their defaults. 32-bit seeds accept
// any safe integer, taken modulo 2^32 (so Date.now() or a negative number is
// a usable seed); NaN and +-Infinity throw. 64-bit seeds take a BigInt (its
// low 64 bits) or a number in [0, 2^64), anything else a RangeError. Values
// read from a callback's RESULT while a native search runs (policy, combat
// and legal actions, obs cells, heuristic picks) never throw mid-rollout:
// an out-of-range one is replaced by the call's fallback, and a combat
// action's moveDir and slots are clamped into their enum / int8 ranges.
// A HexNav field's `ring` (default 64) is clamped to at most 2^20 before it
// sizes the search's bucket array. Limits specific to other calls
// (createVecSimulation's numEnvs 1..65536, factored head sizes, replay files)
// are documented with them.
//
// Quick start:
//   const nav = bro.ai.game.createNavGrid({
//     minX: -20, minZ: -20, maxX: 20, maxZ: 20,
//     cellSize: 0.5,
//     obstacles: [{ x: 0, z: 0, hw: 2, hd: 2 }],
//     padding: 0.4,
//   });
//
//   const bot = bro.ai.game.createAgent({ navGrid: nav, speed: 6, radius: 0.4 });
//   bot.setTarget(10, 5);
//   bot.update(dt);
//   console.log(bot.x, bot.z);
//
// =============================================================================


// -----------------------------------------------------------------------------
// NavGrid, 2D grid-based navigation mesh
// -----------------------------------------------------------------------------

/**
 * Create a navigation grid for pathfinding.
 *
 * @param {Object} opts
 * @param {number} opts.minX - Left bound of navigable area
 * @param {number} opts.minZ - Top bound of navigable area
 * @param {number} opts.maxX - Right bound of navigable area
 * @param {number} opts.maxZ - Bottom bound of navigable area
 * @param {number} [opts.cellSize=0.5] - Grid cell size (smaller = more precise)
 * @param {Array<{x, z, hw, hd}>} [opts.obstacles] - AABB obstacles to mark as blocked
 * @param {number} [opts.padding=0] - Extra clearance around obstacles (agent radius)
 *
 * Cell-count form: pass `width` and `height` in CELLS instead of the four
 * bounds and the extent is derived as origin + cells * cellSize. When both
 * are present they win over minX/minZ/maxX/maxZ.
 * @param {number} [opts.width]    - cells along X (with `height`)
 * @param {number} [opts.height]   - cells along Z
 * @param {number} [opts.originX=0] - world X of cell (0,0) in the cell form
 * @param {number} [opts.originZ=0]
 *
 * Physics bake: derive obstacles from collision geometry so AI and physics
 * can never disagree. Every static, non-sensor body's world-space AABB is
 * projected to XZ and added as an obstacle (with `padding`). Bodies whose XZ
 * footprint covers the entire grid (ground slabs) are skipped automatically.
 * @param {Physics|PhysicsWorldHandle|boolean} [opts.fromPhysics] - the default
 *                                  world (`Physics` or `true`) or a sandbox
 *                                  handle from Physics.createWorldHandle()
 * @param {Array<string|number>} [opts.physicsLayers] - only bake bodies on
 *                                  these collision layers (names or indices)
 * @param {number} [opts.physicsMinY=-Infinity] - only bake bodies whose AABB
 * @param {number} [opts.physicsMaxY=+Infinity]   intersects [minY, maxY]:
 *                                  use to carve out the walkable slab
 * @returns {NavGrid}
 */
const nav = bro.ai.game.createNavGrid({
    minX: -20, minZ: -20, maxX: 20, maxZ: 20,
    cellSize: 0.5,
    obstacles: [
        { x: 0, z: 0, hw: 2, hd: 2 },   // 4x4 box at origin
        { x: 10, z: 5, hw: 1, hd: 3 },   // 2x6 box
    ],
    padding: 0.4,  // agent radius clearance
});

// Bake the level's static collision geometry instead of hand-authoring:
const navBaked = bro.ai.game.createNavGrid({
    minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5,
    fromPhysics: Physics,          // or a Physics.createWorldHandle() sandbox
    physicsLayers: ['static'],     // optional layer filter
    physicsMinY: 0, physicsMaxY: 3, // optional: only the walkable slab
    padding: 0.4,
});

/**
 * Check if a position is on a walkable cell.
 * @param {number} x
 * @param {number} z
 * @returns {boolean}
 */
nav.isWalkable(5, 5);  // true (open space)
nav.isWalkable(0, 0);  // false (inside obstacle)

/**
 * Find a path from start to goal using A* with path smoothing.
 *
 * Partial paths (Godot-style): a blocked, out-of-bounds, or walled-off goal
 * CLAMPS the path to the closest reachable cell instead of failing, the
 * returned array then has `path.partial === true` and ends at the clamped
 * point. `partial` is false on a complete path. Empty only when the START is
 * invalid (out of bounds / on a blocked cell), or when opts.requireFullPath
 * is set and the goal was not reached.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @param {Object} [opts]
 * @param {boolean} [opts.requireFullPath=false] - hard-fail semantics: an
 *     unreached goal returns an empty array instead of a clamped path
 * @returns {Array<{x: number, z: number}> & {partial: boolean}}
 */
const path = nav.findPath(-10, 0, 10, 0);
// path = [{ x: -10, z: 0 }, { x: -2, z: 3 }, { x: 10, z: 0 }], path.partial === false

/**
 * Add an obstacle after creation (e.g., for dynamic obstacles).
 *
 * @param {{x, z, hw, hd}} obstacle - AABB obstacle
 * @param {number} [padding=0] - Extra clearance
 */
nav.addObstacle({ x: 5, z: 5, hw: 1, hd: 1 }, 0.4);

/**
 * Clear the cells an AABB (plus padding) covers back to walkable. This is a
 * blanket un-block, not an undo: it also frees cells that some OTHER
 * obstacle blocks, because a grid cell records walkability, not who blocked
 * it. Re-add the neighbours, or rebuild the grid, when boxes overlap.
 * @param {{x, z, hw, hd}} obstacle
 * @param {number} [padding=0]
 */
nav.removeObstacle({ x: 5, z: 5, hw: 1, hd: 1 }, 0.4);

/**
 * Per-cell edits, for terrain a box cannot describe. Both accept either
 * (x, z, value) or ({x, z}, value).
 * @param {number} x @param {number} z @param {boolean} walkable
 */
nav.setWalkable(3, -2, false);

/**
 * Extra traversal cost multiplier on one cell: A* prefers cheap cells, so a
 * cost above 1 makes a route avoid it (mud, shallow water, a danger zone)
 * without making it impassable.
 * @param {number} x @param {number} z @param {number} cost
 */
nav.setCellCost(3, -2, 4.0);

/**
 * Grid line of sight: is the straight segment free of blocked cells? A 2D
 * Bresenham walk over the grid, not a physics ray.
 * @param {number} fromX @param {number} fromZ @param {number} toX @param {number} toZ
 * @returns {boolean}
 */
nav.hasLineOfSight(-8, 0, 8, 0);        // or nav.hasLineOfSight({x,z}, {x,z})

/**
 * The same test with an object result, for code that also raycasts a navmesh.
 * @returns {{hit: boolean, clear: boolean}} `hit` is `!clear`; there is no
 *     hit point or distance, this grid walk does not compute one.
 */
nav.raycast(-8, 0, 8, 0);

// Grid geometry (all read-only):
nav.width; nav.height;                   // cell counts
nav.cellSize;
nav.minX; nav.minZ; nav.maxX; nav.maxZ;  // world bounds


// -----------------------------------------------------------------------------
// HexNav, weighted pathfinding over a hex grid
// -----------------------------------------------------------------------------
//
// For a game whose world is a pointy-top, odd-r offset hex grid with a cost
// per step that is a *rule* of the game (terrain × locomotion class, walls,
// elevation), not a property of geometry. NavGrid is binary walkable/blocked
// over squares; HexNav searches a cost table the embedder authors — the
// rules stay where they are written, the engine does only the search.
//
// Grid: `size × size` cells, `idx = y * size + x`, directions 0=E 1=NE 2=NW
// 3=W 4=SW 5=SE, odd rows shoved right. A step table is one Float64Array of
// `size*size*6` where slot `c*6 + d` is the cost of ENTERING cell `c` from
// its neighbour in direction `d` (Infinity = impassable). Tables are keyed by
// any string (a locomotion class, say); a clearance table — one byte per
// cell: 1 clear, 2 crushing (doubles the step), anything else cannot be
// stood on — gates a multi-hex footprint on top of a step table.
//
// The searches reproduce a reference JS A* exactly, not just to equal cost:
// the frontier orders by (f, h, insertion sequence), the cost field is single
// precision while keys and sums are double, `maxCost` refuses any step past
// it, the goal is answered when it is settled, and the path is the parent
// chain from the goal. An embedder replacing its own search gets the same
// path for every query, so replays and gates that compare paths hold.
// Deterministic: no threads, no reordered float sums. Typed arrays in and
// out, never a per-cell JS call.

/**
 * Create a hex navigator over a `size × size` grid.
 * @param {Object} opts
 * @param {number} opts.size - cells per side (1..4096)
 * @returns {HexNav}
 */
const hex = bro.ai.game.createHexNav({ size: 256 });
hex.size;   // 256

/**
 * Install a step table under `id` (copied). Replaces any table with that id
 * and drops the cached components for it.
 * @param {string} id - the table's name (e.g. a locomotion class)
 * @param {Float64Array|Float32Array} costs - size*size*6 entry costs
 * @returns {boolean}
 * @throws {RangeError} on a size mismatch
 */
hex.setStepCosts('biped', bipedTable);

/**
 * Rewrite the six entry slots of a few cells in place — a wall breached at
 * runtime touches only its cell and its six neighbours.
 * @param {string} id
 * @param {Int32Array} cells - cell indices
 * @param {Float64Array|Float32Array} values - cells.length * 6 costs, in cell order
 * @returns {boolean} false when `id` has no table
 */
hex.updateStepCosts('biped', new Int32Array([idx, n0, n1]), new Float64Array(18));
hex.hasStepCosts('biped');   // true

/**
 * Install a clearance table under `id` (copied): a Uint8Array of size*size,
 * 1 clear, 2 crushing, else cannot stand.
 * @param {string} id
 * @param {Uint8Array} table
 * @returns {boolean}
 */
hex.setClearance('octopod|8|-1', table);

/**
 * Compute a clearance table natively and install it under `id`: a
 * radius-`radius` footprint (the hex disk) can stand centred on a cell when
 * every footprint cell is in bounds, passable, within ±1 elevation level of
 * the centre, and either has no structure or is crushable — `crushFloors >= 0`
 * and the structure's `floors <= crushFloors` — in which case the cell
 * answers 2 (crushing) rather than 1. Any failure answers 3. A 217-cell
 * walk per cell of a 256 map runs in a few milliseconds.
 * @param {string} id
 * @param {number} radius - footprint radius in cells (0 = the centre alone)
 * @param {Uint8Array} passable - size*size, non-zero where the class may enter
 * @param {Int8Array} elevation - size*size levels
 * @param {Int16Array} floors - size*size storey counts, −1 for no structure
 * @param {number} crushFloors - tallest crushable structure, −1 for never
 * @returns {Uint8Array} the installed table (a copy the caller may keep)
 */
const clr = hex.buildClearance('octopod|8|-1', 8, passable, elevation, floors, -1);
hex.hasClearance('octopod|8|-1');   // true

/**
 * A* over a step table. Cell indices start..goal inclusive, or null when no
 * path costs `maxCost` or less (a goal in another connected component is
 * answered without a search — see components()).
 * @param {string} id - the step table
 * @param {number} x0
 * @param {number} y0
 * @param {number} x1
 * @param {number} y1
 * @param {number} [maxCost=Infinity]
 * @returns {Int32Array|null}
 */
const route = hex.findPath('biped', 12, 12, 200, 190, 400);
// route = Int32Array [ 3084, 3085, ... ]; x = i % size, y = (i / size) | 0

/**
 * The clearance search: the same A* with every destination's step multiplied
 * by its clearance (1 or 2); a destination that cannot be stood on is
 * impassable, and a goal that cannot be stood on answers null.
 * @param {string} id - the step table
 * @param {string} clearanceId - the clearance table
 * @param {number} x0
 * @param {number} y0
 * @param {number} x1
 * @param {number} y1
 * @param {number} [maxCost=Infinity]
 * @returns {Int32Array|null}
 */
const hull = hex.findPathRadius('octopod', 'octopod|8|-1', 35, 35, 220, 220);

/**
 * Dijkstra out from a cell to `maxCost`, over the whole grid: the range wash.
 * @param {string} id
 * @param {number} x0
 * @param {number} y0
 * @param {number} [maxCost=Infinity]
 * @returns {{cost: Float32Array, parent: Int32Array}|null} cost Infinity =
 *   unreached, parent −1 = none; null for an unknown table or a start off
 *   the grid
 */
const wash = hex.movementField('biped', 40, 40, 30);

/**
 * A reversed Dijkstra to a set of target cells: `dist[cell]` is the cost of
 * walking from that cell to the nearest seed, each step paying the entered
 * cell's cost from table `id`, times `auraMult` when the entered cell is in
 * `aura`. `blocked` cells are walls: never relaxed, Infinity. The frontier
 * is a bucket queue in units of `quantum` with a ring of `ring` buckets and
 * FIFO order within a bucket, so a build whose costs are all whole quanta
 * pops in exact distance order with no comparisons; any edge the ring cannot
 * order (off the quantum grid, or longer than the ring) restarts the build on
 * a plain heap — same distances. `pops` counts every frontier pop, stale
 * entries included: an embedder that used to slice this build under a
 * per-tick pop budget can land the finished field on the same tick it
 * would have.
 * @param {string} id - the step table
 * @param {Object} opts
 * @param {Int32Array} opts.seeds - target cells (distance 0), duplicates ignored
 * @param {Uint8Array} [opts.blocked] - size*size wall mask
 * @param {Uint8Array} [opts.aura] - size*size discount mask
 * @param {number} [opts.auraMult=1]
 * @param {number} [opts.quantum=0.25]
 * @param {number} [opts.ring=64]
 * @returns {{dist: Float64Array, parent: Int32Array, pops: number}} parent
 *   is the neighbour a cell's distance came through (−1 for seeds and
 *   unreached cells)
 */
const lane = hex.field('biped', { seeds, blocked, aura, auraMult: 0.5, quantum: 0.25, ring: 64 });

/**
 * Weakly connected components of a table's finite edges (with a clearance
 * table: of the edges whose destination can be stood on), one label per
 * cell numbered in first-visit order. Two cells with different labels have
 * no path between them in either direction. Cached per (table, clearance)
 * and rebuilt after the table changes.
 * @param {string} id
 * @param {string} [clearanceId]
 * @returns {Int32Array}
 */
const comp = hex.components('biped');
comp[a] === comp[b];   // false ⇒ findPath(a → b) is null without a search


// -----------------------------------------------------------------------------
// NavMesh, polygon navigation mesh (Recast/Detour)
// -----------------------------------------------------------------------------
//
// The 3D counterpart to NavGrid, for worlds a flat 2D grid cannot represent:
// slopes, ramps, bridges/overpasses, multi-level interiors. Baked from
// arbitrary triangle soup; all points are y-up world-space {x, y, z}.
//
// NavGrid vs NavMesh: use a NavGrid for flat single-level arenas. It is
// instant to build, supports dynamic obstacles (addObstacle after creation),
// and backs the avoidance wall bridge. Use a NavMesh when the level has
// height: walkable slopes, stacked floors, or interiors, the bake voxelizes
// real geometry, erodes by agent radius, and findPath returns 3D waypoints.
// A default bake is static: rebake (or cache + reload) when the level
// changes. For doors, crates, and spawned walls, bake with
// `dynamicObstacles: true` instead, that enables the runtime obstacle API
// (see "Dynamic obstacles" below) with incremental tile rebuilds, no rebake.
//
// Availability: requires a build configured with
// -DBROGAMEAGENT_WITH_NAVMESH=ON (Recast/Detour from vcpkg). Feature-detect
// with `bro.ai.game.navMeshAvailable`, when false, bakeNavMesh/loadNavMesh
// throw.

/**
 * Bake a polygon navmesh from any mix of geometry sources. All requested
 * sources are concatenated into one triangle soup and baked once. Baking is
 * seconds-scale for big levels: cache the result with navMesh.save() and
 * restore with loadNavMesh() at startup.
 *
 * Triangles must be wound counter-clockwise when viewed from above (+Y
 * normals) to be considered walkable.
 *
 * Geometry sources (combinable):
 * @param {Float32Array|number[]} [opts.positions] - flat xyz vertex triples
 * @param {Uint32Array|number[]}  [opts.indices]   - triangle index list
 *
 * @param {Physics|PhysicsWorldHandle|boolean} [opts.fromPhysics] - collect
 *     every static, non-sensor body's actual triangle geometry (the default
 *     world via `Physics`/`true`, or a Physics.createWorldHandle() sandbox).
 *     Mesh and heightfield shapes contribute their exact triangles;
 *     primitive/convex shapes (box/sphere/capsule/hull) contribute Jolt's
 *     coarse triangulation: a box is 12 triangles, spheres/capsules a
 *     low-LOD tessellation. Fine for navigation, not render-accurate.
 * @param {Array<string|number>} [opts.physicsLayers] - only collect bodies on
 *     these collision layers (names or indices)
 *
 * @param {Terrain} [opts.fromTerrain] - a scene.createTerrain() handle. The
 *     terrain's top surface is height-sampled on a regular grid (one down-
 *     raycast per sample) over `terrainBounds`: slopes and plateaus bake
 *     accurately; caves/overhangs are approximated by the top surface.
 *     Chunks must be streamed in (terrain.update) before baking.
 * @param {{minX, minZ, maxX, maxZ}} [opts.terrainBounds] - required with
 *     fromTerrain: the XZ region to sample
 * @param {number} [opts.terrainStep=1.0] - sample spacing (world units)
 * @param {number} [opts.terrainRayStart=100] - probe ray start height
 * @param {number} [opts.terrainRayLength=200] - probe ray length
 *
 * Bake config (Recast semantics, tuned for a ~0.5 m radius humanoid in
 * meter-scale worlds):
 * @param {number} [opts.cellSize=0.25]       - XZ voxel size (~agentRadius/2)
 * @param {number} [opts.cellHeight=0.2]      - Y voxel size
 * @param {number} [opts.agentRadius=0.5]     - walkable area eroded by this
 * @param {number} [opts.agentHeight=2.0]     - min clearance for a span
 * @param {number} [opts.agentMaxClimb=0.4]   - max step/ledge height
 * @param {number} [opts.agentMaxSlopeDeg=45] - steeper triangles unwalkable
 * @param {number} [opts.regionMinSize=8]     - min region size (cells)
 * @param {number} [opts.regionMergeSize=20]  - merge-into-neighbor threshold
 * @param {number} [opts.edgeMaxLen=12]       - max contour edge length
 * @param {number} [opts.edgeMaxError=1.3]    - contour simplification (cells)
 * @param {number} [opts.detailSampleDist=6]  - detail-mesh sampling (cells)
 * @param {number} [opts.detailSampleMaxError=1] - detail-mesh max deviation
 *
 * Off-mesh links (Godot NavigationLink analog: see "Off-mesh links"):
 * @param {Array<Object>} [opts.offMeshLinks] - point-to-point traversal
 *     shortcuts baked into the mesh: jump gaps, drop ledges, ladders,
 *     teleporters. Each: {start: {x,y,z}, end: {x,y,z}, radius?,
 *     bidirectional?, userId?}. Static bakes only, combining with
 *     dynamicObstacles fails the bake (tile rebuilds would drop the links).
 *
 * Dynamic obstacles (tiled bake: see the "Dynamic obstacles" section):
 * @param {boolean} [opts.dynamicObstacles=false] - bake TILED via Detour's
 *     dtTileCache so obstacles can be added/removed at runtime. Trade-offs vs
 *     the default static bake: no detail mesh (waypoint Y is quantized to
 *     cellHeight: slightly coarser on slopes), no save() serialization, and
 *     small disconnected islands (e.g. crate tops) survive instead of being
 *     culled by regionMinSize. Bake time is comparable; queries are the same.
 * @param {number} [opts.tileSize=16] - tile edge length (world units),
 *     clamped to 16..255 cells. Smaller tiles = cheaper per-obstacle rebuild
 *     but more tiles; an obstacle may span at most 8 tile-layers, so keep
 *     tileSize at least about half your largest obstacle's footprint.
 * @param {number} [opts.maxObstacles=128] - obstacle slot budget
 *
 * @returns {NavMesh}
 * @throws {Error} on bake failure, with the Recast build log in the message
 */
const navMesh = bro.ai.game.bakeNavMesh({
    fromPhysics: Physics,          // level collision geometry...
    positions: rampVerts,          // ...plus extra hand-authored soup
    indices: rampIndices,
    agentRadius: 0.5,
    agentMaxClimb: 0.4,
});

/**
 * Find a walkable path. Returns the straightened (funnel) waypoint list as a
 * Float32Array of xyz triples: [x0,y0,z0, x1,y1,z1, ...], including the
 * snapped start and end points, with a `partial` bool property.
 *
 * Partial paths (Godot-style): an UNREACHABLE goal (disconnected island)
 * clamps the path to the closest reachable point: the result then has
 * `partial === true` and its last triple is the clamped end, not the goal.
 * Complete paths read `partial === false`. Returns null only when either
 * endpoint fails to snap within the extents, or when requireFullPath is set
 * and the goal is unreachable.
 * Deterministic: same mesh + inputs always yield the same waypoints.
 *
 * The third argument is either bare extents or an options object:
 *
 * @param {{x,y,z}|number[]} start
 * @param {{x,y,z}|number[]} end
 * @param {{x,y,z}|Object} [extentsOrOpts]
 * @param {{x,y,z}} [extentsOrOpts.extents={x:2,y:1,z:2}] - snap-box
 *     half-extents. The tight Y is deliberate: it makes stacked-level
 *     queries resolve to the level nearest the query point. Keep it smaller
 *     than your level spacing.
 * @param {boolean} [extentsOrOpts.requireFullPath=false] - hard-fail
 *     semantics: an unreachable goal returns null instead of a clamped path
 * @returns {(Float32Array & {partial: boolean})|null}
 */
const wp = navMesh.findPath({ x: -8, y: 0, z: 0 }, { x: 8, y: 3, z: 0 });
if (wp) for (let i = 0; i < wp.length; i += 3) walkTo(wp[i], wp[i + 1], wp[i + 2]);
if (wp && wp.partial) console.log('goal unreachable, walking to the closest point');

// Off-mesh link markers: `wp.links` is an array of POINT indices that are
// link takeoffs, the segment from point i to point i+1 traverses the link
// (jump/drop/teleport), not the walkable surface. Empty when the path uses
// no links. wp.links = [2] means the segment wp[6..8] → wp[9..11] is a jump.

/**
 * Snap an arbitrary point onto the navmesh.
 * @param {{x,y,z}|number[]} p
 * @param {{x,y,z}} [extents]
 * @returns {{x,y,z}|null} null when nothing is within the search extents
 */
navMesh.nearestPoint({ x: 0, y: 10, z: 0 });

/**
 * Walkability raycast from start toward end ALONG the mesh surface (2D
 * boundary test, not a physics ray): does a straight walk get there, and if
 * not, where does it stop?
 * @param {{x,y,z}|number[]} start - must snap onto the mesh
 * @param {{x,y,z}|number[]} end
 * @param {{x,y,z}} [extents]
 * @returns {{hit: boolean, t: number, point: {x,y,z}, normal: {x,y,z}}}
 *     hit=true when a boundary blocked the ray before `end`; t is the hit
 *     param along [start,end] (1 when unobstructed); normal is the XZ wall
 *     normal at the hit (zero when unobstructed).
 */
const ray = navMesh.raycast({ x: 0, y: 0, z: 0 }, { x: 10, y: 0, z: 0 });

/**
 * Uniform-ish random reachable point on the mesh (area-weighted polygon
 * pick). Deterministic per seed.
 * @param {number} seed
 * @returns {{x,y,z}|null} null when the mesh is empty
 */
navMesh.randomPoint(42);

// Aliases kept so code written against either spelling works:
navMesh.findRandomPoint(42);                  // === randomPoint
navMesh.closestPoint({ x: 0, y: 10, z: 0 });  // === nearestPoint
navMesh.samplePosition({ x: 0, y: 10, z: 0 }); // === nearestPoint

/**
 * Re-bake this mesh in place from raw triangle soup, discarding whatever it
 * held. bakeNavMesh() is the usual entry point; this is for re-baking one
 * handle as a level streams in.
 * @param {Float32Array|number[]} positions - flat xyz triples
 * @param {Uint32Array|number[]} indices    - triangle index list
 * @param {Object} [opts] - only the core six are read here: cellSize,
 *     cellHeight, agentRadius, agentHeight, agentMaxClimb, agentMaxSlopeDeg
 * @returns {NavMesh} this mesh
 * @throws {Error} on a failed bake, with the Recast log in the message
 */
navMesh.buildFromMesh(positions, indices, { agentRadius: 0.5 });

/** Whether a bake/load has succeeded (read-only). */
navMesh.valid;

/**
 * Serialize the baked mesh (raw self-validating blob).
 * @returns {ArrayBuffer}
 */
const blob = navMesh.save();

/**
 * Restore a mesh previously produced by save(). Loading is a cheap memcpy,
 * the bake is the expensive part, so the standard recipe is: bake once,
 * cache to disk, load at startup.
 * @param {ArrayBuffer|TypedArray} buffer
 * @returns {NavMesh}
 * @throws {Error} on malformed data
 */
const fs = require('fs');
let cached;
try { cached = fs.readFileSync('level.navmesh'); } catch (e) {}
const mesh = cached
    ? bro.ai.game.loadNavMesh(cached.buffer)
    : (() => {
        const m = bro.ai.game.bakeNavMesh({ fromPhysics: Physics });
        fs.writeFileSync('level.navmesh', Buffer.from(m.save()));
        return m;
      })();

// --- Dynamic obstacles (doors, crates, spawned walls) ---
//
// Available on meshes baked with `dynamicObstacles: true`. Obstacles carve
// the walkable surface exactly like baked-in geometry: findPath detours
// around them (or clamps to the closest reachable point with partial=true
// when they sever the corridor), and removing them restores the original
// surface.
//
// Update semantics: addObstacle/removeObstacle only QUEUE a change. The
// affected tiles rebuild incrementally, one touched tile per update() call,
// and the engine pumps update() automatically once per frame, so a change
// takes effect over the next few frames (typically 1-2). `generation` bumps
// once per applied batch; navigating agents (node.navigateTo) watch it and
// repath automatically, an agent whose corridor gets blocked detours, and
// one whose goal becomes unreachable halts instead of ghost-walking the
// stale route. Call `while (!mesh.update()) {}` only when a change must
// apply synchronously (e.g. right before a findPath in the same tick).
//
// Limits: an obstacle may span at most 8 tile-layers (size obstacles ≲
// 2 tiles across, or raise tileSize); at most 64 add/remove requests may be
// queued between pumps (addObstacle throws "request queue full" beyond
// that); obstacle slots are capped by bakeNavMesh's maxObstacles.

const dyn = bro.ai.game.bakeNavMesh({
    fromPhysics: Physics,
    dynamicObstacles: true,   // tiled dtTileCache bake
    tileSize: 16,
    maxObstacles: 128,
});

/**
 * Add an obstacle. Three shapes:
 *   {type: 'cylinder', pos, radius, height}   pos = center of the BASE
 *   {type: 'box', min, max}                   axis-aligned box
 *   {type: 'box', center, halfExtents, yaw?}  Y-rotated box (yaw in radians)
 * @returns {number} handle for removeObstacle()
 * @throws {Error} on a static (non-dynamicObstacles) mesh, a malformed
 *     descriptor, a full request queue, or exhausted obstacle slots
 */
const door = dyn.addObstacle({
    type: 'box', min: { x: 4, y: 0, z: -1 }, max: { x: 5, y: 3, z: 1 },
});
const crate = dyn.addObstacle({
    type: 'cylinder', pos: { x: -3, y: 0, z: 2 }, radius: 0.8, height: 1.5,
});

/**
 * Queue removal. Returns false for unknown/stale handles (double-remove is a
 * clean no-op).
 * @param {number} handle
 * @returns {boolean}
 */
dyn.removeObstacle(door);

/**
 * Pump pending changes by hand: rebuilds at most one touched tile per call,
 * returns true once fully up to date. The engine already pumps once per
 * frame: use this only for synchronous application.
 * @param {number} [dt=1/60] - forwarded to Detour (currently unused by it)
 * @returns {boolean}
 */
while (!dyn.update()) {}          // apply everything right now

/** Whether this mesh was baked with dynamicObstacles (read-only). */
dyn.supportsObstacles;

/** Active obstacles, added and not removed, including queued (read-only). */
dyn.obstacleCount;

/** True while queued changes have not been fully applied yet (read-only). */
dyn.obstaclesPending;

/**
 * Monotonic surface version (read-only): bumps after bake/load and once per
 * applied obstacle batch. Poll it to know when a change has landed; agent
 * bindings use it for automatic repath.
 */
dyn.generation;

// --- Off-mesh links (jump gaps, drop ledges, ladders, teleporters) ---
//
// The Godot NavigationLink analog: a point-to-point shortcut baked into the
// mesh at bakeNavMesh time. Path queries traverse links automatically.
// Detour routes through them like any polygon, and the result marks each
// takeoff point (findPath's `links` indices; navigationInfo().onLink for a
// routed agent) so apps can play a jump/climb animation while the agent
// moves straight along the link segment.
//
// Semantics:
//   - Each endpoint must land within `radius` of the (eroded) walkable
//     surface; a link whose endpoint misses is silently dropped, exactly
//     like a Godot link placed off the mesh.
//   - `bidirectional: false` makes the link one-way (start → end), think
//     drop-down ledges.
//   - Links live in the baked Detour data, so save()/loadNavMesh() keeps
//     them.
//   - Limitation (honest): links are NOT available on dynamicObstacles
//     (tiled) bakes, dtTileCache rebuilds tiles at runtime and would drop
//     bake-time connections, so bakeNavMesh throws instead of losing them
//     silently. Bake a static mesh for linked levels.
//   - Agents (node.navigateTo) traverse a link by moving straight from
//     takeoff to landing (Y interpolates linearly along the segment when no
//     groundFollow probe is set; with groundFollow the node's Y keeps
//     tracking the probed ground, drive the jump arc yourself off onLink
//     if you want airtime). Repaths are deferred while onLink so a mid-air
//     position is never re-snapped.

const linked = bro.ai.game.bakeNavMesh({
    positions: verts, indices: idx,
    offMeshLinks: [
        { start: { x: -2, y: 0, z: 0 }, end: { x: 2, y: 0, z: 0 }, radius: 0.6 },
        { start: { x: 5, y: 3, z: 0 }, end: { x: 5, y: 0, z: 2 }, bidirectional: false },
    ],
});
const lp = linked.findPath({ x: -8, y: 0, z: 0 }, { x: 8, y: 0, z: 0 });
if (lp) for (const i of lp.links) {
    console.log('jump from', lp[i * 3], lp[i * 3 + 1], lp[i * 3 + 2],
                'to', lp[i * 3 + 3], lp[i * 3 + 4], lp[i * 3 + 5]);
}

// --- Agent routing over a navmesh ---
//
// node.navigateTo() drives an attached agent along NavMesh::findPath
// waypoints using the agent's existing setTarget/followPath steering (XZ),
// so the AI world's ORCA avoidance pass (world.setAvoidance) composes
// unchanged, routed agents still locally avoid each other. Waypoint Y is
// interpolated along the active segment and drives the node's height when no
// groundFollow probe is set; groundFollow, when set, wins. While a route is
// active the binding owns the agent's movement target (a think-hook moveTo
// issued the same tick is overridden); the route ends on arrival or
// stopNavigation().
//
// Dynamic obstacles: the binding snapshots the mesh's `generation` at plan
// time and re-plans automatically when the surface changes (an obstacle
// batch applied, or a re-bake). A goal that becomes unreachable clamps the
// route to the closest reachable point (navigationInfo().partial turns
// true); with requireFullPath the route is abandoned and the agent halts
// instead, issue a fresh navigateTo() after the blocking obstacle is
// removed.

const world = bro.ai.game.createWorld();
world.setAvoidance(true);
const agent = bro.ai.game.createAgent({ x: -8, z: 0, speed: 4, avoidance: true });
scene.attachAIWorld(world, { stepHz: 60 });

const node = scene.createMesh({ mesh: 'box' });
node.attachAgent(world, agent, {
    navMesh,          // enables navigateTo on this node
    yOffset: 0.5,     // clearance above the route height
});

/**
 * Plan a path on the bound navmesh from the agent's position to `target`
 * and start following it.
 *
 * Partial routes (Godot-style): an UNREACHABLE goal still starts a route,
 * the agent walks to the closest reachable point and stops there.
 * navigationInfo().partial reads true for such a route, and the agent's
 * `atTarget` stays false at the clamped end (it measures against the true
 * goal). There is no separate completion event, poll navigationInfo():
 * `!active` after a partial route started means the agent finished at the
 * clamped end. Pass requireFullPath to fail instead of clamping.
 *
 * @param {{x,y,z}|number[]} target
 * @param {Object} [opts]
 * @param {NavMesh} [opts.navMesh] - bind/replace the navmesh (optional if
 *     one was passed to attachAgent)
 * @param {{x,y,z}} [opts.extents] - findPath snap half-extents
 * @param {number} [opts.repathInterval=0] - seconds; > 0 re-plans toward the
 *     same target periodically (0 = plan once per navigateTo call)
 * @param {boolean} [opts.requireFullPath=false] - unreachable goal returns
 *     false and the agent does not move, instead of a partial route
 * @returns {boolean} true when following started (complete OR partial
 *     route); false when an endpoint fails to snap, or, with
 *     requireFullPath: when the goal is unreachable
 */
node.navigateTo({ x: 8, y: 3, z: 0 });

/** Abandon the current route (the agent halts). */
node.stopNavigation();

/**
 * State of the binding's navmesh route.
 * @returns {{active: boolean, partial: boolean, onLink: boolean}}
 *     active = a route is being followed; partial = the active/most-recent
 *     route was clamped to the closest reachable point (goal unreachable),
 *     persists after arrival until the next navigateTo()/stopNavigation()
 *     so late polls can still see how the route ended; onLink = the agent
 *     is currently traversing an off-mesh link segment (watch the
 *     transition to play jump/climb animations).
 */
node.navigationInfo();


