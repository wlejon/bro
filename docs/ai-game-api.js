// =============================================================================
// bro.ai.game, Game AI API Reference
// =============================================================================
//
// The game AI API provides server-side pathfinding, steering, and perception
// for building game bots. Backed by the brogameagent C++ library.
//
// Available in all modes (windowed, headless, server).
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


// -----------------------------------------------------------------------------
// Agent, Pathfinding + steering combined
// -----------------------------------------------------------------------------

/**
 * Create a game agent that navigates using a NavGrid.
 *
 * @param {Object} [opts]
 * @param {NavGrid} [opts.navGrid] - Navigation grid for pathfinding
 * @param {number} [opts.x=0] - Initial X position
 * @param {number} [opts.z=0] - Initial Z position
 * @param {number} [opts.speed=6] - Movement speed (units/second)
 * @param {number} [opts.radius=0.4] - Collision radius
 * @param {number} [opts.elevation=0] - Vertical position (Y) for the ORCA
 *                                  multi-level elevation filter (see bot.elevation)
 * @param {boolean|Object} [opts.avoidance] - ORCA participation/tuning (see
 *                                  "Local avoidance" below)
 * @returns {Agent}
 */
const bot = bro.ai.game.createAgent({
    navGrid: nav,
    x: -16, z: -16,
    speed: 6,
    radius: 0.4,
});

/**
 * Set the movement target. Automatically computes a path via the NavGrid.
 * Recomputes if the target moves significantly (>2 units).
 * @param {number} x
 * @param {number} z
 */
bot.setTarget(10, 5);

/** Clear the target. Agent stops moving. */
bot.clearTarget();

/**
 * Advance the agent by dt seconds. Moves along the current path
 * using steering behaviors (seek for intermediate waypoints, arrive for final).
 * @param {number} dt - Delta time in seconds
 */
bot.update(1 / 60);

/** Teleport the agent to a new position. */
bot.setPosition(0, 0);

/**
 * Compute aim yaw/pitch from the agent's position to a 3D world point.
 * Uses -Z forward convention (yaw=0 faces -Z).
 *
 * @param {number} targetX
 * @param {number} targetY
 * @param {number} targetZ
 * @param {number} eyeHeight - Agent's eye height above ground
 * @returns {{ yaw: number, pitch: number }}
 */
const aim = bot.aimAt(enemyX, 1.6, enemyZ, 1.6);

/** Current X position (read-only). */
bot.x;

/** Current Z position (read-only). */
bot.z;

/**
 * Vertical position (Y) for multi-level worlds: read/write. Only feeds the
 * ORCA elevation filter (see avoidance.height): agents on different levels
 * don't steer around each other. Movement itself stays XZ. Also accepted as
 * `elevation:` in createAgent(opts). Scene-attached agents with groundFollow
 * or a navmesh route get it updated automatically from the surface height.
 */
bot.elevation = 4;

/** Current facing direction in radians (read-only). 0 = -Z, positive = clockwise. */
bot.yaw;

/** Whether the agent has an active target (read-only). */
bot.hasTarget;

/** Whether the agent has reached its target (read-only). */
bot.atTarget;


// -----------------------------------------------------------------------------
// Local avoidance (ORCA), agents stop walking through each other
// -----------------------------------------------------------------------------
//
// Optimal Reciprocal Collision Avoidance, solved natively inside
// world.tick(). Off by default (agents keep the legacy pass-through-each-
// other movement). When enabled, each living agent's path-following
// steering becomes its *preferred* velocity, the ORCA solver filters it
// against nearby agents (the pair splits the avoidance effort by their
// priorities, 50/50 by default, see avoidance.priority) and static walls,
// and the filtered velocity drives the agent's usual dynamics (maxAccel /
// maxTurnRate / nav-grid clamping still apply). avoidance.layers/mask
// scope who avoids whom. Deterministic: the same roster + obstacles +
// ticks replay identically.
//
// Scene-attached agents get this for free, attachAIWorld ticks the same
// world, so think() callbacks issuing self.moveTo(...) produce paths that
// flow around other agents.

/**
 * Enable/disable the avoidance pass on a world.
 *
 * Walls: the world's own addObstacle() AABBs are always respected while
 * avoidance is on. Passing a navGrid additionally bases avoidance-only
 * walls on that grid's obstacle boxes, so agents locally steer around the
 * exact geometry A* paths around, and since createNavGrid({fromPhysics})
 * bakes obstacles from static physics bodies, that composition makes
 * avoidance physics-aware for free. Boxes are copied (no reference kept);
 * call again after mutating the grid.
 *
 * @param {boolean|Object} opts - boolean, or:
 * @param {boolean} [opts.enabled=true]
 * @param {NavGrid} [opts.navGrid] - rebase avoidance walls on this grid's obstacles
 */
world.setAvoidance(true);
world.setAvoidance({ navGrid: nav });   // enable + respect the grid's walls
world.setAvoidance(false);

/** Whether the avoidance pass is enabled (read-only). */
world.avoidanceEnabled;

/**
 * Per-agent participation + tuning. Also accepted as `avoidance:` in
 * createAgent(opts) and node.attachAgent(world, agent, opts).
 * `false` opts the agent out: it keeps legacy (unfiltered) movement but
 * others still steer around it at full effort: good for bosses or
 * player-driven units that shouldn't yield.
 *
 * @param {boolean|Object} opts - boolean, or:
 * @param {boolean} [opts.enabled=true]
 * @param {number}  [opts.radius]            - avoidance disc radius (default: agent radius)
 * @param {number}  [opts.maxSpeed]          - speed cap on the solved velocity (default: agent speed)
 * @param {number}  [opts.neighborDist=10]   - only agents within this range are considered
 * @param {number}  [opts.maxNeighbors=10]   - nearest-N cap on the neighbor set
 * @param {number}  [opts.timeHorizon=2]     - seconds of mutual lookahead vs agents
 * @param {number}  [opts.timeHorizonObst=1] - seconds of lookahead vs walls
 * @param {number}  [opts.height=2]          - vertical extent for the multi-level
 *   elevation filter: agents whose spans [elevation - height/2, elevation +
 *   height/2] don't overlap are on different levels (bridge over tunnel,
 *   stacked floors) and ignore each other. Elevations default to 0, so
 *   single-level worlds never filter.
 * @param {number}  [opts.priority=0.5]      - avoidance responsibility weight,
 *   0..1. When two agents negotiate, each takes the effort share
 *       share = clamp(0.5 + 0.5 * (otherPriority - selfPriority), 0, 1),
 * shares sum to 1 across the pair, so ORCA's collision-free reciprocity
 *   is preserved. Equal priorities keep the classic 50/50; the LOWER-priority
 *   agent takes proportionally more, and at the extremes (1 vs 0) the
 *   low-priority agent does all the avoiding while the high-priority one
 *   holds course (think boss vs minions). Deterministic.
 * @param {number}  [opts.layers=1]          - layer membership bitmask (which
 *   avoidance layers this agent occupies)
 * @param {number}  [opts.mask=1]            - neighbor-selection bitmask: agent
 *   A avoids neighbor B only when (A.mask & B.layers) !== 0. One-sided by
 *   design: B may still avoid A if B's mask matches A's layers, and an agent
 *   avoiding a neighbor that cannot see it back automatically takes the FULL
 *   effort (no reciprocity to count on). Default 1/1 = everyone avoids
 *   everyone.
 */
bot.setAvoidance({ radius: 0.5, timeHorizon: 2.5 });
// A boss that plows through the crowd; minions scatter:
boss.setAvoidance({ priority: 1.0 });
// Ghosts and the living never steer around each other:
ghost.setAvoidance({ layers: 2, mask: 2 });

// Typical setup: shared nav grid + avoidance, then just set targets.
const arena = bro.ai.game.createNavGrid({
    minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5,
    obstacles: [{ x: 0, z: 0, hw: 3, hd: 0.5 }],
});
const world2 = bro.ai.game.createWorld();
world2.setAvoidance({ navGrid: arena });
const a1 = bro.ai.game.createAgent({ navGrid: arena, x: -10, z: 0, avoidance: { radius: 0.5 } });
const a2 = bro.ai.game.createAgent({ navGrid: arena, x: 10, z: 0 });
world2.addAgent(a1); world2.addAgent(a2);
a1.setTarget(10, 0); a2.setTarget(-10, 0);   // they pass, not overlap
// world2.tick(dt) each frame, or scene.attachAIWorld(world2).


// -----------------------------------------------------------------------------
// Perception, Line of sight, aim computation
// -----------------------------------------------------------------------------

/**
 * 2D line-of-sight check through AABB obstacles.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @param {Array<{x, z, hw, hd}>} obstacles - AABBs to test against
 * @returns {boolean} true if line is clear
 */
const clear = bro.ai.game.hasLineOfSight(
    botX, botZ, enemyX, enemyZ,
    obstacles
);

/**
 * 2D field-of-view and line-of-sight check through AABB obstacles.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @param {number} facingYaw - Facing direction in radians (0 = -Z)
 * @param {number} fovRadians - Field of view arc in radians (e.g. Math.PI / 2 for 90°)
 * @param {number} maxRange - Sight range limit in world units
 * @param {Array<{x, z, hw, hd}>} obstacles - AABBs to test against
 * @returns {boolean} true if target is within FOV arc, within maxRange, and unobstructed
 */
const canSee = bro.ai.game.canSee(
    botX, botZ, enemyX, enemyZ,
    facingYaw, Math.PI / 2, 15, obstacles
);

/**
 * Compute aim angles from one 3D point to another.
 * Uses -Z forward convention.
 *
 * @param {number} fromX
 * @param {number} fromY
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toY
 * @param {number} toZ
 * @returns {{ yaw: number, pitch: number }}
 */
const aim2 = bro.ai.game.computeAim(0, 1.6, 0, 10, 1.6, -5);

/**
 * Lead a moving target with a finite-speed projectile. Solves for the future
 * intercept point and returns the aim angles to that point, plus a `valid`
 * flag (false if no real intercept exists, e.g. target outrunning projectile)
 * and the predicted time-of-flight.
 *
 * @param {number} fromX  @param {number} fromY  @param {number} fromZ
 * @param {number} targetX @param {number} targetY @param {number} targetZ
 * @param {number} targetVX @param {number} targetVY @param {number} targetVZ
 * @param {number} projectileSpeed
 * @returns {{ yaw: number, pitch: number, valid: boolean, timeToHit: number }}
 */
const lead = bro.ai.game.computeLeadAim(
    0, 1.6, 0,             // shooter
    10, 1.6, -5,           // target pos
    -2, 0, 1,              // target velocity
    40);                   // projectile speed
if (lead.valid) bot.fire(lead.yaw, lead.pitch);


// -----------------------------------------------------------------------------
// bro.ai.game.steer.*, pure-function steering primitives
// -----------------------------------------------------------------------------
//
// Stateless 2D steering kernels. Each returns a desired-velocity direction
// `{fx, fz}` (NOT normalized), the caller integrates it into actual motion
// (multiply by speed * dt, clamp, blend, etc.). Use these inside custom
// think() callbacks or scripted policies when Agent's built-in path-following
// isn't the right behavior. All positions and velocities are XZ-plane.

/** Move directly toward `target` at full desired speed.
 *  @returns {{fx: number, fz: number}} */
const s1 = bro.ai.game.steer.seek(selfX, selfZ, targetX, targetZ);

/** Seek with deceleration once within `slowingRadius` of `target`. Used
 *  by Agent for the final waypoint so it doesn't overshoot.
 *  @param {number} slowingRadius */
const s2 = bro.ai.game.steer.arrive(selfX, selfZ, targetX, targetZ, 1.5);

/** Move directly away from a threat point. */
const s3 = bro.ai.game.steer.flee(selfX, selfZ, threatX, threatZ);

/** Lead a moving target. Seeks the predicted future position assuming
 *  constant target velocity. `selfSpeed` sets the lookahead horizon.
 *  @param {number} targetVX @param {number} targetVZ @param {number} selfSpeed */
const s4 = bro.ai.game.steer.pursue(
    selfX, selfZ, targetX, targetZ, targetVX, targetVZ, selfSpeed);

/** Inverse of pursue, flee from the threat's predicted future position.
 *  @param {number} threatVX @param {number} threatVZ @param {number} selfSpeed */
const s5 = bro.ai.game.steer.evade(
    selfX, selfZ, threatX, threatZ, threatVX, threatVZ, selfSpeed);


// -----------------------------------------------------------------------------
// Capability / policy / AgentBinding, scene-driven AI
// -----------------------------------------------------------------------------
//
// A scene node can own an "agent binding", a capability set (the tools this
// object can use) plus a JS think(self, world) callback (how it decides).
// Minions, towers, and heroes all use the same binding shape; behaviour
// differs only by which capabilities are enabled and which think() fn is
// supplied. Difficulty scales along three orthogonal knobs:
//   1. capability set, add or remove tools
//   2. thinkHz, how often the decision fires
//   3. think fn, simple scripted, MCTS wrapper, or NN policy
//
// Built-in capabilities (string ids used in opts.capabilities):
//   "move_to", self.moveTo(x, z) sets the pathfinding target
//   "lane_walk", self.laneWalk() steps through opts.laneWaypoints
//   "basic_attack", self.attack(targetId); blocks for 1/attacksPerSec
//   "cast_ability", self.cast(slot, targetId); blocks for cast time (~0.25s)
//   "flee", self.flee([x, z]); retreats away from nearest enemy
//   "hold", self.hold([dur]); no-op for dur seconds (always exposed)
//
// A `self` proxy is built fresh each think tick. It only exposes methods
// whose capability is present on the binding, towers won't have .moveTo.
//
// Custom capabilities (registerCapability, below) have no dedicated
// self.<name>() accessor of their own, invoke them with:
//   self.useCapability(name, arg0?, arg1?)
// exposed whenever the binding's capabilities list contains at least one
// registerCapability'd id. arg0/arg1 land in the capability's Action as
// i0/i1 (same slots self.attack/self.cast use for target/slot ids) but
// aren't visible to gate/start/advance below. Read them via a JS-side
// closure handoff instead, same as self.attack/self.cast's targets aren't
// passed to the *native* capability callbacks either.


/**
 * Register a JS-authored capability. Returns the assigned capability id.
 * Callbacks run from C++; keep them fast. `start` is invoked when the
 * capability is chosen; `advance` each frame while it's in flight; `gate`
 * when building the available-capability mask (optional, default true).
 *
 * @param {string} name
 * @param {Object} spec
 * @param {function} [spec.gate]     - () => boolean
 * @param {function} [spec.start]    - () => void
 * @param {function} [spec.advance]  - () => boolean (true = done)
 * @param {number}   [spec.id]       - optional explicit id (default: auto-allocated from 100+)
 * @returns {number} capability id
 */
bro.ai.game.registerCapability("kite", {
    gate()    { return true; },
    start()   { /* ... */ },
    advance() { return true; /* done this tick */ },
});


// -----------------------------------------------------------------------------
// SceneGraph.attachAIWorld, auto-tick a World each frame
// -----------------------------------------------------------------------------

/**
 * Drive a brogameagent::World from the engine frame loop at a fixed step.
 * Replaces the JS-side accumulator pattern (see broworkshop's ai/ai-arena/main.js).
 *
 * @param {AIWorld} world
 * @param {Object}  [opts]
 * @param {number}  [opts.stepHz=60]            - fixed-step rate
 * @param {number}  [opts.maxStepsPerFrame=8]   - catch-up clamp for stalls
 */
scene.attachAIWorld(world, { stepHz: 60, maxStepsPerFrame: 8 });
scene.detachAIWorld();


// -----------------------------------------------------------------------------
// SceneNode.attachAgent, bind an AI agent to a scene object
// -----------------------------------------------------------------------------

/**
 * Attach an AI agent + capability set to a scene node. The binding takes
 * care of steering, combat, aim, cast timing, and writing transforms. Call
 * attachAIWorld first (the binding reads the world via the scene graph's
 * attached ticker; the `world` arg here is the JS wrapper passed to think).
 *
 * @param {AIWorld} world
 * @param {AIAgent} agent
 * @param {Object}  [opts]
 * @param {string[]} [opts.capabilities] - ids of enabled caps (default: all built-ins)
 * @param {function(self, world): void} [opts.think] - imperative decision fn
 * @param {number}  [opts.thinkHz=15]
 * @param {number}  [opts.yOffset=0]    - node Y (absolute), or clearance above
 *                                        the ground when groundFollow is set
 * @param {boolean} [opts.faceMovement=true]
 * @param {Array<{x,z}>} [opts.laneWaypoints] - waypoints for lane_walk
 * @param {string}  [opts.policy]       - "scripted_minion" as a C++ fallback
 * @param {boolean|Object} [opts.avoidance] - ORCA participation/tuning for the
 *                                        bound agent (see "Local avoidance"
 *                                        above); effective while the world has
 *                                        world.setAvoidance(true)
 * @param {NavMesh} [opts.navMesh]      - bind a bakeNavMesh()/loadNavMesh()
 *                                        handle, enabling node.navigateTo()
 *                                        route-following (see "Agent routing
 *                                        over a navmesh" above)
 *
 * Ground follow: agents plan in 2D (x, z); groundFollow makes the bound
 * node's Y track the ground under the agent instead of sitting at a constant
 * height. The probe runs natively once per frame; when it has no answer
 * (chunk not streamed in / nothing under the ray) the node keeps the last
 * known ground height.
 * @param {Object}  [opts.groundFollow]
 * @param {string}  opts.groundFollow.mode - 'terrain' (sample a voxel terrain's
 *                                  surface) or 'raycast' (physics down-raycast
 *                                  against the default world's static geometry)
 * @param {Terrain} [opts.groundFollow.terrain] - required for mode 'terrain':
 *                                  a scene.createTerrain() object
 * @param {number}  [opts.groundFollow.rayStart=100] - world Y the down-probe starts from
 * @param {number}  [opts.groundFollow.rayLength=200] - probe length below rayStart
 * @param {Array<string|number>} [opts.groundFollow.layers] - raycast mode: only
 *                                  hit bodies on these collision layers
 */
minionNode.attachAgent(world, minionAgent, {
    capabilities: ["lane_walk", "basic_attack", "hold"],
    thinkHz: 10,
    laneWaypoints: [{ x: -15, z: 0 }, { x: 0, z: 0 }, { x: 15, z: 0 }],
    think(self, w) {
        const e = w.nearestEnemy(self.agent);
        if (e && self.inRange(e)) return self.attack(e.unit.id);
        return self.laneWalk();
    },
});

// Walk the terrain: yOffset becomes clearance above the sampled surface.
scoutNode.attachAgent(world, scoutAgent, {
    yOffset: 0.5,
    groundFollow: { mode: 'terrain', terrain: myTerrain },
    think(self) { return self.moveTo(30, -12); },
});

// Or follow physics floors/platforms via a downward raycast.
guardNode.attachAgent(world, guardAgent, {
    yOffset: 1.0,
    groundFollow: { mode: 'raycast', layers: ['static'] },
    think(self) { return self.hold(1); },
});

/** Remove the binding; the node stops receiving AI updates. */
minionNode.detachAgent();


// -----------------------------------------------------------------------------
// The `self` proxy passed to think()
// -----------------------------------------------------------------------------
//
// Built fresh each think tick. Read-only snapshots of unit state plus
// imperative methods for the enabled capabilities. Exactly one method call
// per think determines the next action (last call wins).
//
// Read-only:
//   self.hp / mana / x / z / id / teamId / attackRange / alive
//   self.agent, the underlying AIAgent (for world queries)
//
// Universal helpers (always available):
//   self.distanceTo(target), target may be {x,z}, AIAgent, or self
//   self.inRange(target [, range]), default range is self.attackRange
//   self.hold([dur]), no-op fallback
//
// Capability methods (present only when the cap is enabled):
//   self.moveTo(x, z)                   // if move_to
//   self.laneWalk()                     // if lane_walk
//   self.attack(targetId)               // if basic_attack
//   self.cast(slot, targetId)           // if cast_ability
//   self.flee([x, z])                   // if flee
//
// A tower ({capabilities:["basic_attack","hold"]}) has no .moveTo / .laneWalk;
// attempting to call them throws. A think() that falls through without
// picking an action defaults to .hold().


// -----------------------------------------------------------------------------
// MCTS planners
// -----------------------------------------------------------------------------
//
// Seven flavors, all sharing the same MctsConfig:
//
//   createMcts(cfg), single agent vs scripted opponents
//   createDecoupledMcts(cfg), simultaneous-move 1v1 (both sides searched)
//   createTeamMcts(cfg), cooperative N-hero joint planner
//   createTacticMcts(cfg), coarse team-tactic planner (Hold / FocusLowestHp / ...)
//   createLayeredPlanner({ tactic, fine }).
// TacticMcts over TeamMcts with a tactic-match prior
//   createOptionMcts(cfg), search over caller-authored single-hero Options
//                               (temporally-extended macro-actions)
//   createTeamOptionMcts(cfg), team-scoped option search
//
// MctsConfig fields (all optional):
//   iterations, budgetMs, rolloutHorizon, simDt, actionRepeat, uctC, seed,
//   pwAlpha, progressive widening α (0 disables)
//   priorC, PUCT weight (0 ⇒ plain UCT with uctC)
//   tacticWindowDecisions, LayeredPlanner / TacticMcts only
//   optionMaxWindows, OptionMcts / TeamOptionMcts only; cap
//                                    on in-tree option execution length
//   useLeafValue, skip rollout entirely and return the
//                                    evaluator's value at the expand site.
//                                    Pair with a strong heuristic/learned
//                                    evaluator to decouple depth from cost.
//   rolloutPolicy : "random" | "aggressive" | "scripted"
//                 | function(selfView, worldView) => CombatAction
//   opponentPolicy: "idle"   | "aggressive" | "scripted"
//   prior         : "uniform" | "attackBias" | "tacticMatch"
//                 | function(selfView, worldView, actions[]) => weights[]
//                   (tacticMatch reads `tactic`, `tacticMatchWeight`,
//                    `tacticOtherWeight`)
//   evaluator     : "hpDelta"      (hero-scoped)
//                 | "teamHpDelta" | "teamAdvantage" | "teamPosition"  (team)
//                 | function(worldView, heroId|teamId) => number in [-1, 1]
//
// JS callbacks receive plain-object views:
//   selfView/agentView : { id, teamId, x, z, yaw, hp, maxHp, alive, attackRange }
//   worldView          : { agents: [agentView, ...] }
// Rollout/prior run many times per search; prefer C++ presets for hot paths.
//
// A CombatAction is { moveDir, attackSlot, abilitySlot }.
// A Tactic is { kind: "Hold" | "FocusLowestHp" | "Scatter" | "Retreat" }.
// Move direction ints are exposed at bro.ai.game.MOVE_DIR, tactic strings at
// bro.ai.game.TACTIC.

// Single-agent: one hero vs scripted rest
const mcts = bro.ai.game.createMcts({
    iterations: 800, budgetMs: 10, rolloutHorizon: 24,
    rolloutPolicy: "aggressive", opponentPolicy: "aggressive",
    prior: "attackBias", evaluator: "hpDelta",
});
const action = mcts.search(world, hero);
mcts.advanceRoot(action);
console.log(mcts.lastStats); // { iterations, bestVisits, elapsedMs, reusedRoot, ... }

// Decoupled 1v1: both sides searched simultaneously (no opponentPolicy)
const duel = bro.ai.game.createDecoupledMcts({ iterations: 1500, prior: "attackBias" });
const joint1v1 = duel.search(world, hero, opp); // { hero: CombatAction, opp: CombatAction }
duel.advanceRoot(joint1v1.hero, joint1v1.opp);

// Cooperative team: one planner, joint action per hero
const team = bro.ai.game.createTeamMcts({
    iterations: 1200, rolloutHorizon: 20,
    rolloutPolicy: "aggressive", opponentPolicy: "aggressive",
    evaluator: "teamHpDelta",
});
const perHero = team.search(world, heroes); // [CombatAction, ...]  (length = heroes.length)
team.advanceRoot(perHero);

// Hierarchical: tactic every N windows + fine per-hero every call
const planner = bro.ai.game.createLayeredPlanner({
    tactic: { iterations: 300, budgetMs: 4, tacticWindowDecisions: 4, actionRepeat: 4 },
    fine:   { iterations: 600, budgetMs: 6, actionRepeat: 4, priorC: 2.0 },
    rolloutPolicy: "aggressive",
    opponentPolicy: "aggressive",
    evaluator: "teamHpDelta",
});
const groupActions = planner.decide(world, heroes); // [CombatAction, ...]
console.log(planner.committedTactic.kind);          // "FocusLowestHp", etc.
console.log(planner.windowsUntilReplan);
console.log(planner.lastStats.fineStats.iterations);

// Root-parallel: N native threads, each with its own Mcts over its own
// World, joined and merged into one action. Caller must hand in an array of
// pre-built Worlds already seeded to the SAME game state. This does not
// clone `world` for you (a World doesn't own its Agents, so cloning would be
// unsafe to do implicitly). Blocking call: all worker threads are fully
// joined before it returns, so QuickJS never observes concurrency and there
// is no JS-side locking to worry about. Because of that same threading,
// `evaluator`/`rolloutPolicy` MUST be a string preset or a native/neural
// wrapper (e.g. bro.ai.game.learn.createNeuralEvaluator(...)), a plain JS
// function is rejected with a TypeError, since N native threads calling
// back into QuickJS concurrently would be unsafe.
const parallelWorlds = [world1, world2, world3, world4]; // same state, N clones
const { action: pAction, stats: pStats } = bro.ai.game.rootParallelSearch({
    worlds: parallelWorlds, heroId: hero.id,
    iterations: 200, rolloutHorizon: 12, actionRepeat: 4, seed: 0xDEAD,
    evaluator: "hpDelta", rolloutPolicy: "aggressive", opponentPolicy: "aggressive",
});
console.log(pStats); // { numThreads, totalIterations, elapsedMs, mergedBestVisits }

// Decoupled root-parallel: same threading contract, no opponentPolicy (both
// sides searched simultaneously, like createDecoupledMcts).
const { hero: pHero, opp: pOpp } = bro.ai.game.rootParallelSearchDecoupled({
    worlds: parallelWorlds, heroId: hero.id, oppId: opp.id,
    iterations: 200, evaluator: "hpDelta", rolloutPolicy: "aggressive",
});

// Helpers for custom planners / UI debug:
const legalA = bro.ai.game.legalActions(hero, world);     // [CombatAction, ...]
const legalT = bro.ai.game.legalTactics(world, heroes);   // [Tactic, ...]
const concrete = bro.ai.game.tacticToAction(              // Tactic → CombatAction
    { kind: bro.ai.game.TACTIC.FocusLowestHp }, hero, world);


// ─── Options (temporally-extended macro-actions) ──────────────────────────
//
// An Option is a policy with initiation + termination predicates. OptionMcts
// plans at the granularity of options rather than per-tick CombatActions,
// branching collapses from ~18 to the size of the option set, and each tree
// edge covers many windows, so the effective horizon multiplies by option
// length. The right tool for multi-tick maneuvers (peek/shoot, retreat to
// cover, flank) that plain search can't plan cheaply at realtime budgets.

// Author a single-hero option. All three callbacks run synchronously inside
// MCTS search, keep them allocation-light.
const peekAndShoot = bro.ai.game.createOption({
    name: "peekAndShoot",
    canInitiate:     (self, world) =>
        world.agents.some(a => a.alive && a.teamId !== self.teamId
                               && Math.hypot(a.x - self.x, a.z - self.z) < 12),
    step:            (self, world, ticks) => {
        const enemy = world.agents.find(a => a.alive && a.teamId !== self.teamId);
        return { moveDir: ticks < 2 ? 3 /*E*/ : 7 /*W*/,
                 attackSlot: enemy ? 0 : -1, abilitySlot: -1 };
    },
    shouldTerminate: (self, world, ticks) => ticks >= 4 || self.hp < self.maxHp * 0.3,
});
const retreat = bro.ai.game.createOption({
    name: "retreatToCover",
    canInitiate:     (self) => self.hp < self.maxHp * 0.5,
    step:            ()     => ({ moveDir: 10 /*PathAway*/, attackSlot: -1, abilitySlot: -1 }),
    shouldTerminate: (self, _w, ticks) => ticks >= 5 || self.hp > self.maxHp * 0.8,
});
const hold = bro.ai.game.createOption({
    name: "hold",
    canInitiate:     ()     => true,
    step:            ()     => ({ moveDir: 0, attackSlot: 0, abilitySlot: -1 }),
    shouldTerminate: (_s, _w, ticks) => ticks >= 2,
});

const opt = bro.ai.game.createOptionMcts({
    iterations: 80, rolloutHorizon: 3, optionMaxWindows: 6,
    options: [peekAndShoot, retreat, hold],
    opponentPolicy: "scripted",
    evaluator: "hpDelta",
    useLeafValue: true,                 // skip random rollouts; use eval at leaf
});
const chosen = opt.search(world, hero); // "peekAndShoot" | "retreatToCover" | "hold" | null
if (chosen) {
    opt.executeOption(world, hero, chosen);  // advance the live world
    opt.advanceRoot(chosen);                  // reuse tree next call
}

// Team variant, callbacks receive heroesView[] and step returns a
// CombatAction[] (one per hero, in order).
const teamPush = bro.ai.game.createTeamOption({
    name: "push",
    canInitiate: (heroes, world) =>
        world.agents.some(a => a.alive && heroes[0] && a.teamId !== heroes[0].teamId),
    step: (heroes) => heroes.map(() => ({ moveDir: 9 /*PathToTarget*/, attackSlot: 0, abilitySlot: -1 })),
    shouldTerminate: (_h, _w, ticks) => ticks >= 4,
});
const teamOpt = bro.ai.game.createTeamOptionMcts({
    iterations: 60, optionMaxWindows: 4,
    options: [teamPush],
    evaluator: "teamAdvantage",
    opponentPolicy: "scripted",
});
const chosenTeam = teamOpt.search(world, heroes);  // option name or null


// ─── Commander (role-based hierarchical team planner) ────────────────────
//
// Assigns heroes to roles (lead / flank / support / etc.); each role owns
// an option set and optional per-role evaluator. Per-hero OptionMcts runs
// within the role's option space, no joint-action combinatorial search.
// Role re-assignment fires every `replanEveryWindows` decide() calls, or
// whenever the team composition changes.
//
// This is the "group thinking" primitive. Use it when joint-space search
// (TeamMcts / TeamOptionMcts) is too wide at your iteration budget.

const leadOpts    = [peekAndShoot, hold];
const supportOpts = [retreat, hold];
const cmdr = bro.ai.game.createCommander({
    replanEveryWindows: 3,
    roleCfg: { iterations: 80, rolloutHorizon: 3, optionMaxWindows: 4, useLeafValue: true },
    opponentPolicy: "scripted",
    evaluator: "hpDelta",                  // default for roles without their own
    roles: [
        { name: "lead",    options: leadOpts },
        { name: "support", options: supportOpts,
          evaluator: (worldView, heroId) => {
              // Support values ally HP preservation over damage dealt.
              const me = worldView.agents.find(a => a.id === heroId);
              if (!me) return 0;
              const allies = worldView.agents.filter(a => a.teamId === me.teamId && a.alive);
              const avg = allies.reduce((s, a) => s + a.hp / a.maxHp, 0) / Math.max(1, allies.length);
              return avg * 2 - 1;
          } },
    ],
    // Optional custom assigner. Default is round-robin by hero index.
    assign: (heroes, world) => heroes.map((h, i) => h.hp < h.maxHp * 0.4 ? 1 : 0),
});

const groupActions2 = cmdr.decide(world, heroes); // [CombatAction, ...]
console.log(cmdr.currentAssignments);             // [0, 1, 0, ...] role indices
console.log(cmdr.committedOption(0));             // "peekAndShoot" | null
console.log(cmdr.windowsUntilReplan);
console.log(cmdr.roles);                          // [{name, optionCount}, ...]


// -----------------------------------------------------------------------------
// NN training: observation, action mask, reward tracker
// -----------------------------------------------------------------------------
//
// The pieces a learned policy needs at every tick: an ego-centric float
// observation, a validity mask over the discrete action heads, and a per-
// agent reward delta. All three are zero-allocation on the C++ side and
// hand back typed arrays / plain objects to JS.

/**
 * Build the ego-centric observation vector for `agent`. Layout (constants
 * exposed as `bro.ai.game.OBS_TOTAL`):
 *   self block    (14 floats: hp, mana, attack/ability cooldowns, speed,
 *                  sin/cos(aim - yaw))
 *   enemy block   (5 nearest enemies × 6 floats: valid, relX, relZ, dist,
 *                  hp%, inAttackRange)
 *   ally block    (4 nearest allies × 5 floats: valid, relX, relZ, dist, hp%)
 * Positions are in agent's local frame, normalized by OBS_RANGE (50 units).
 *
 * @param {AIAgent} agent
 * @param {AIWorld} world
 * @returns {Float32Array} length = bro.ai.game.OBS_TOTAL
 */
const obs = bro.ai.game.buildObservation(focusAgent, world);
UI.drawObservation(obs);

/**
 * Build the legal-action mask for `agent`. Use to renormalize a policy
 * softmax over only currently-valid choices.
 *
 * Layout (`bro.ai.game.MASK_TOTAL` floats, 1.0 = legal, 0.0 = illegal):
 *   [0 .. N_ENEMY_SLOTS)   "attack enemy in slot k", slot k matches the
 *                          k-th enemy in the observation (nearest-first).
 *                          `enemyIds[k]` is the underlying Unit::id (or -1).
 *   [N_ENEMY_SLOTS ..)     "cast ability slot s", bound + cooldown ready
 *                          + mana sufficient (range not checked).
 *
 * @param {AIAgent} agent
 * @param {AIWorld} world
 * @returns {{ mask: Float32Array, enemyIds: Int32Array }}
 */
const am = bro.ai.game.buildActionMask(focusAgent, world);
const legalAttacks = am.enemyIds.filter((id, k) => id >= 0 && am.mask[k] > 0);

/**
 * Per-agent reward-delta accumulator. Captures the agent's baseline at
 * construction; each `consume()` call returns the delta since the previous
 * call and re-latches. Reads `world.events()`. Do not call
 * `world.clearEvents()` in between consume() calls.
 *
 * @param {AIAgent} agent
 * @param {AIWorld} world
 * @returns {RewardTracker}
 */
const tracker = bro.ai.game.createRewardTracker(agent, world);

/** @returns {{damageDealt, damageTaken, kills, deaths, distanceTravelled}} */
const d = tracker.consume(agent, world);
const r = d.damageDealt - d.damageTaken + d.kills * 20 - d.deaths * 20;

/** Re-latch the baseline (e.g. start of a new episode). */
tracker.reset(agent, world);


// -----------------------------------------------------------------------------
// Headless training harness: bro.ai.game.createSimulation(world)
// -----------------------------------------------------------------------------
//
// Fixed-dt rollout driver for offline NN training and offscreen sims. Wraps
// brogameagent::Simulation. Per step(dt): registered policies fire, results
// are applied via World::applyAction, then World::tick advances scripted
// agents and projectiles. Agents WITHOUT a registered policy keep their
// scripted behaviour (lane walk, basic attack, etc.).
//
// The simulation does not own the world; the caller controls lifetime.

/**
 * @param {AIWorld} world
 * @returns {Simulation}
 */
const sim = bro.ai.game.createSimulation(world);

/**
 * Register a policy for one agent. Called every step with the agent and a
 * world view; must return an AgentAction
 *   { moveX, moveZ, aimYaw, aimPitch, attackTargetId, abilitySlot, abilityTargetId }.
 *
 * @param {number} agentId  - Unit::id of the controlled agent
 * @param {function(agent, world): AgentAction} policy
 */
sim.addPolicy(heroAgent.unit.id, function (self, w) {
    const obs  = bro.ai.game.buildObservation(self, w);
    const mask = bro.ai.game.buildActionMask(self, w);
    return myPolicy.forward(obs, mask);   // your NN inference
});

/** Stop driving this agent. It falls back to scripted World::tick. */
sim.removePolicy(heroAgent.unit.id);

/** One fixed-dt step. */
sim.step(1 / 60);

/** Convenience: call step(dt) `n` times. Headless training inner loop. */
sim.runSteps(1 / 60, 600);          // 10 sim-seconds at 60Hz

sim.steps;                          // total steps taken (read-only)
sim.elapsed;                        // total sim seconds (read-only)
sim.resetCounters();                // does NOT reset world state


// -----------------------------------------------------------------------------
// Replay I/O: createRecorder() / createReplayReader()
// -----------------------------------------------------------------------------
//
// Streaming binary format (.bgar). Recorder writes per-frame agent state +
// damage events + a frame index appended on close() so any frame can be
// random-accessed. ReplayReader opens a finished file and exposes per-frame
// snapshots, per-agent trajectories, and a damage summary.

/** @returns {Recorder} */
const rec = bro.ai.game.createRecorder();

/**
 * Open a file for writing. `dt` is recorded in the header for playback.
 * @param {string} path
 * @param {number} episodeId
 * @param {number} seed
 * @param {number} dt
 * @returns {boolean} false on I/O error
 */
rec.open(path, 1, Date.now(), 1 / 60);
rec.isOpen;                                       // true

/** Write the static roster (one entry per agent). Call once before frames.
 *  @param {AIWorld} world */
rec.writeRoster(world);

/** Capture one frame: agents, projectiles, and the slice of world.events()
 *  that arrived since the last recordFrame. Do NOT call world.clearEvents()
 *  between recordFrame calls or that window's events are lost.
 *  @param {number} stepIdx @param {number} elapsed @param {AIWorld} world */
rec.recordFrame(state.steps, state.elapsed, world);

rec.frameCount;                                    // frames written so far
rec.close();                                       // appends index + footer

/** @returns {ReplayReader} */
const rr = bro.ai.game.createReplayReader();
if (!rr.open(path)) UI.log("open failed: " + rr.errorMessage);
rr.frameCount;                                     // total frames

/** Random-access one frame.
 *  @returns {{ stepIdx, elapsed,
 *              agents: [{id, x, z, hp, mana, yaw, alive}, ...],
 *              events: [{attackerId, targetId, amount, killed}, ...] }} */
const f = rr.frame(0);

/** Full XZ trajectory for one agent across the replay.
 *  @returns {[{stepIdx, elapsed, x, z, hp, alive}, ...]} */
const traj = rr.trajectory(heroAgent.unit.id);

/** Aggregate damage / hits / kills per (attacker, target) pair.
 *  @returns {[{attackerId, targetId, totalDamage, hits, kills}, ...]} */
const dmg = rr.damageSummary();


// =============================================================================
// bro.ai.game.nn, Neural network primitives
// =============================================================================
//
// Thin bindings over brogameagent::nn. Intended for training loops and custom
// value/policy networks. Most users will compose SingleHeroNet and plug it
// into a NeuralEvaluator / NeuralPrior (see bro.ai.game.learn) rather than
// hand-wiring circuits.
//
// All Tensor arguments are *owned* JS objects; ops mutate them in place.
//
//   const t = bro.ai.game.nn.createTensor(rows, cols?);
//   t.rows, t.cols, t.size                            // read-only shape
//   t.zero(); t.resize(r, c);                         // fill / reshape
//   t.get(r, c); t.set(r, c, v);                      // per-element
//   t.toArray() -> Float32Array                        // copy out
//   t.fromArray(Float32Array)                          // copy in
//   t.copyFrom(otherTensor)                            // deep copy
//
// Circuit classes expose forward/backward, save/load (Uint8Array blobs),
// and zeroGrad/sgdStep. Each circuit owns its own cache for backward.

/** @type {Tensor} */
const W = bro.ai.game.nn.createTensor(4, 3);

/** Linear (dense) layer. W:(out,in), b:(out). */
const lin = bro.ai.game.nn.createLinear(inDim, outDim, seed);
lin.forward(x, y);            // y = W·x + b
lin.backward(dY, dX);         // accumulates dW, dB; produces dX
lin.zeroGrad(); lin.sgdStep(0.01, 0.9);
lin.W; lin.b; lin.dW; lin.dB; // Tensor views (copies)
const blob = lin.save();      // Uint8Array
lin.load(blob);

const relu = bro.ai.game.nn.createRelu();
const tanh = bro.ai.game.nn.createTanh();

/** DeepSetsEncoder, permutation-invariant self+enemies+allies encoder. */
const enc = bro.ai.game.nn.createDeepSetsEncoder({hidden: 32, embedDim: 32}, seed);
enc.outDim;                    // 3 * embedDim
enc.forward(obsVec, embed);

/** Value / factored-policy heads. */
const vHead = bro.ai.game.nn.createValueHead(embedDim, hidden, seed);
const val = vHead.forward(embed);   // scalar in [-1,1]
vHead.backward(dValue, dEmbed);

const pHead = bro.ai.game.nn.createFactoredPolicyHead(embedDim, seed);
pHead.totalLogits;                  // N_MOVE + N_ATTACK + N_ABILITY
pHead.forward(embed, logits);
pHead.backward(dLogits, dEmbed);

/** PolicyValueNet, generic small MLP with value head and a single (flat)
 *  policy head, decoupled from the MOBA-shaped observation/action layout
 *  that SingleHeroNet assumes. Use this when your observation is hand-crafted
 *  and your action space is a small flat set of discrete choices (e.g.
 *  platformer buttons, gridworld moves, puzzle-game pieces).
 *
 *  Architecture:
 *    in_dim → hidden[0] → ReLU → ... → hidden[n-1] → { value(tanh), logits }
 *
 *  Forward returns the scalar value; logits are written into the supplied
 *  Tensor. Backward expects (dValue, dLogits) where dLogits is typically
 *  (probs - target) from nn.softmaxXent (with optional legal-action mask).
 *
 *  Wire format magic differs from SingleHeroNet: blobs are not interchangeable.
 */
const pvnet = bro.ai.game.nn.createPolicyValueNet({
    inDim: 60,
    hidden: [64, 64],         // any non-empty list of positive ints
    valueHidden: 32,
    numActions: 6,
    seed: 0xC0DE1234n,
});
pvnet.inDim; pvnet.numActions; pvnet.trunkDim; pvnet.numParams;
const valuePV = pvnet.forward(obsTensor, logitsTensor);     // returns scalar
pvnet.backward(dValuePV, dLogitsTensor);
pvnet.zeroGrad(); pvnet.sgdStep(lr, momentum);
const pvBlob = pvnet.save();   pvnet.load(pvBlob);

// forwardBatched(x, logits, values), B rows in one call (one dispatch
// instead of B), for interleaving multiple searches' leaf evaluations
// gathered within a single JS tick. x is (B, inDim); logits/values are
// pre-sized by the caller as (B, numActions) / (B, 1). Same shape on
// SingleHeroNetTX below, SingleHeroNet does NOT have this method (it
// doesn't implement the underlying BatchedNet interface).
const xB = bro.ai.game.nn.createTensor(8, pvnet.inDim);
const logitsB = bro.ai.game.nn.createTensor(8, pvnet.numActions);
const valuesB = bro.ai.game.nn.createTensor(8, 1);
pvnet.forwardBatched(xB, logitsB, valuesB);

/** SingleHeroNet, encoder → trunk → {value, policy}. */
const net = bro.ai.game.nn.createSingleHeroNet({
  enc: { hidden: 32, embedDim: 32 },
  trunkHidden: 64,
  valueHidden: 32,
  seed: 0xC0DEn,
});
const v2 = net.forward(x, logits);   // returns scalar value
net.backward(dValue, dLogits);
net.zeroGrad(); net.sgdStep(lr, momentum);
net.embedDim; net.trunkDim; net.policyLogits; net.numParams;
const blob2 = net.save();            // Uint8Array
net.load(blob2);

/** WeightsHandle, atomic publish/snapshot of net weights across threads. */
const handle = bro.ai.game.nn.createWeightsHandle();
handle.publish(blob2, 1n);           // blob = Uint8Array, version = BigInt
const snap = handle.snapshot();      // { blob: Uint8Array, version: BigInt } | null
handle.version();

/** Primitive ops (same signatures as the C++ header). */
bro.ai.game.nn.linearForward(W, b, x, y);
bro.ai.game.nn.linearBackward(W, x, dY, dX, dW, dB);
bro.ai.game.nn.reluForward(x, y);     bro.ai.game.nn.reluBackward(x, dY, dX);
bro.ai.game.nn.tanhForward(x, y);     bro.ai.game.nn.tanhBackward(y, dY, dX);
bro.ai.game.nn.softmaxForward(logits, probs, maskOrNull);
bro.ai.game.nn.softmaxBackward(probs, dProbs, dLogits);
/** @returns {number} loss */
const loss = bro.ai.game.nn.softmaxXent(logits, target, probs, dLogits, maskOrNull);
/** @returns {{loss, dPred}} */
const mseR = bro.ai.game.nn.mseScalar(pred, target);
bro.ai.game.nn.addInplace(y, x);  bro.ai.game.nn.addScalarInplace(y, s);
/** @returns {BigInt} advanced seed state */
const seed2 = bro.ai.game.nn.xavierInit(W, 0xC0DEn);
bro.ai.game.nn.factoredSoftmax(logits, probs, atkMaskOrNull, abilMaskOrNull);
const fLoss = bro.ai.game.nn.factoredXent(
  logits, targetMove, targetAttack, targetAbility,
  probs, dLogits, atkMaskOrNull, abilMaskOrNull);

// Constants
bro.ai.game.nn.N_MOVE;   // 9
bro.ai.game.nn.N_ATTACK; // N_ENEMY_SLOTS + 1
bro.ai.game.nn.N_ABILITY;// N_ABILITY_SLOTS + 1


// =============================================================================
// bro.ai.game.learn, Training infrastructure
// =============================================================================

/** NeuralEvaluator, IEvaluator adapter wrapping a SingleHeroNet. Pass as
 *  the `evaluator` option in any Mcts/InfoSetMcts config. */
const neuralEval = bro.ai.game.learn.createNeuralEvaluator(net, handle);
neuralEval.evaluate(world, heroId);    // scalar in [-1,1]

/** NeuralPrior, IPrior adapter. Pass as `prior` in Mcts/InfoSetMcts config. */
const neuralPrior = bro.ai.game.learn.createNeuralPrior(net, handle);
neuralPrior.setTemperature(1.0);
neuralPrior.setUniformMix(0.05);

/** GumbelNoisePrior, wraps an inner prior and adds IID Gumbel noise at the
 *  root for exploration under small MCTS budgets. */
const gumbel = bro.ai.game.learn.createGumbelNoisePrior(neuralPrior, /*scale*/ 1.0);
gumbel.reseed(0xA11CEn);
gumbel.setScale(1.0);

/** Situation, training example as a plain object.
 *  {obs, atkMask, abilMask, targetMove, targetAttack, targetAbility, valueTarget} */

/** ReplayBuffer, fixed-capacity FIFO of situations. */
const buf = bro.ai.game.learn.createReplayBuffer(/*capacity*/ 4096);
buf.push(situation);
buf.size; buf.capacity;
const batch = buf.sample(32);
const all = buf.all(); buf.clear();

/** ExItTrainer, mini-batch SGD+momentum against (value, policy) targets. */
const trainer = bro.ai.game.learn.createExItTrainer();
trainer.setNet(net);
trainer.setBuffer(buf);
trainer.setWeightsHandle(handle);
trainer.setConfig({
  lr: 0.01, momentum: 0.9, batch: 32,
  policyWeight: 1.0, valueWeight: 1.0,
  publishEvery: 100,
  rngSeed: 0x1234n,
});
const step = trainer.step();         // {lossValue, lossPolicy, lossTotal, samples}
const stepN = trainer.stepN(100);
trainer.totalSteps; trainer.totalPublishes;

/** Extract AlphaZero-style training targets from a completed Mcts search.
 *  @returns {{move: Float32Array, attack: Float32Array, ability: Float32Array}}
 *  or null if the tree is empty. */
const targets = bro.ai.game.learn.targetsFromMcts(mcts);

/** Build a Situation from a completed search. value_target is left at 0,
 *  the caller fills it with the eventual episode return before pushing. */
const sit = bro.ai.game.learn.makeSituation(mcts, hero, world);
sit.valueTarget = finalReturn;
buf.push(sit);

/** Gumbel-improved policy target (Danihelka 2022, simplified). */
const tgt2 = bro.ai.game.learn.gumbelImprovedPolicy(mcts);


// ─── Generic ExIt: arbitrary obs / flat discrete action space ─────────────
//
// Pair PolicyValueNet with the generic replay buffer + trainer when your
// problem doesn't fit the combat-shaped Situation (no enemy slots, no
// ability cooldowns, no factored heads). Same SGD+momentum loop, same
// WeightsHandle hot-swap; differs only in the Situation shape and which
// net it drives.

/** GenericSituation, plain JS object the buffer accepts:
 *    {
 *      obs:          Float32Array(net.inDim),
 *      policyTarget: Float32Array(net.numActions),  // soft distribution, sums to ≈ 1
 *      actionMask?:  Float32Array(net.numActions),  // 1.0 legal, 0.0 illegal; omit ⇒ all legal
 *      valueTarget:  number in [-1, 1]              // typically discounted return, clipped
 *    }
 */

const gbuf = bro.ai.game.learn.createGenericReplayBuffer(/*capacity*/ 4096);
gbuf.push({ obs: o, policyTarget: pi, actionMask: mask, valueTarget: 0.7 });
gbuf.size; gbuf.capacity;
const gbatch = gbuf.sample(32);   // [GenericSituation, ...]
const gall   = gbuf.all();        gbuf.clear();

const gtrainer = bro.ai.game.learn.createGenericExItTrainer();
gtrainer.setNet(pvnet);
gtrainer.setBuffer(gbuf);
gtrainer.setWeightsHandle(handle);
gtrainer.setConfig({
    lr: 0.01, momentum: 0.9, batch: 32,
    policyWeight: 1.0, valueWeight: 1.0,
    publishEvery: 100,
    rngSeed: 0x1234n,
});
const gstep   = gtrainer.step();        // {lossValue, lossPolicy, lossTotal, samples}
const gstepN  = gtrainer.stepN(100);
gtrainer.totalSteps; gtrainer.totalPublishes;


// ─── Batched GPU inference (BatchedInferenceServer / IInferenceBackend) ────
//
// Batching only pays off under CONCURRENT callers, a single-threaded JS
// caller always submits one observation at a time (batch-of-1 through the
// server). For batching multiple observations gathered within a single JS
// tick, use net.forwardBatched(x, logits, values) above instead. That's
// the actual single-threaded-JS win. These bindings exist for completeness
// and forward-compat with future concurrent callers (e.g. a root-parallel
// GenericMcts sharing one server), and to power the GenericMcts `backend`
// fast path below.
//
// net must be a PolicyValueNet or SingleHeroNetTX (both implement the
// underlying BatchedNet interface), SingleHeroNet does not, and is
// rejected with a TypeError.

const server = bro.ai.game.learn.createInferenceServer(pvnet, {
    maxBatchSize: 64,       // default 64
    maxWaitMicros: 500,     // default 500, how long to wait for a batch to fill
});
const r1 = server.evaluate(obsF32);              // blocking: { logits: Float32Array, value }
const rows = server.evaluateBatch([obsF32, obsF32Other]);  // [{ logits, value }, ...]
server.batchesRun;      // int
server.shutdown();      // stop the server's worker thread

// Direct (no server/threading, evaluates net inline, same process/thread)
// or server-backed IInferenceBackend, for plugging into GenericMcts's
// `backend` option (see below):
const directBackend = bro.ai.game.learn.createDirectBackend(pvnet);
const serverBackend = bro.ai.game.learn.createServerBackend(server, pvnet);
directBackend.numActions; directBackend.inDim;

// GenericMcts native `backend` option, masked-softmax prior + value
// evaluated entirely in C++, no per-node JS-callback round trip (the env's
// snapshot/restore/step/legalActions/observe callbacks are still JS, since
// the env itself is JS-authored; only the prior/value leaf evaluation moves
// native). An explicit priorFn/valueFn always wins over `backend` if both
// are given.
//
// IMPORTANT: this is NOT a drop-in for the hero-Mcts `evaluator`/`prior`
// config slot on createMcts/createDecoupledMcts/createTeamMcts/etc. Those
// take combat-shaped IEvaluator/IPrior. A backend plugs in one layer down,
// at GenericMcts's own prior_fn/value_fn (this section), over a flat
// action space matching net.numActions.
const gm = bro.ai.game.createGenericMcts({
    env: myGenericEnv,
    backend: directBackend,   // or serverBackend
    iterations: 400,
});


// =============================================================================
// Belief / observability / Information-Set MCTS
// =============================================================================

/** VisibilityConfig is a plain object:
 *   { fovRadians?: number, maxRange?: number, checkLos?: boolean } */

/** Build a fresh TeamObservation against ground truth. Allies are fully
 *  known; enemies are visible iff any living ally has LOS+FOV+range.
 *  @returns {{teamId, timestamp, allies, enemies}} */
const teamObs = bro.ai.game.observe(world, teamId, visCfg, /*now*/ simTime);

/** Merge a fresh observation into a prior one, carrying stale enemies
 *  forward with lastSeenElapsed updated. */
const merged = bro.ai.game.mergeObservations(prior, fresh, now);

/** TeamBelief, per-team particle cloud over hidden enemy state. */
const tb = bro.ai.game.createTeamBelief({
  teamId: 0, numParticles: 32, navGrid: nav,
  motion: { maxSpeed: 6, accelStd: 4, spreadOnLoss: 3 },
  seed: 0xBE11Fn,
});
tb.registerEnemy(enemyId, maxHp, /*initialPos*/ {x:0,z:0});
tb.propagate(world, visCfg, dt);
tb.update(teamObs);
const particles = tb.sample();        // { [enemyId]: {x,z,vx,vz,hp,heading,weight} }
const means = tb.mean();
tb.ess;                                // effective sample size
tb.enemies();                          // per-enemy {enemyId, visible, everSeen, ...}
tb.teamId; tb.numParticles; tb.clear();

/** InfoSetMcts, IS-MCTS for single-hero under partial observability. */
const isMcts = bro.ai.game.createInfoSetMcts();
isMcts.setBelief(tb);
isMcts.setEvaluator("hpDelta");       // string preset, function, or object
isMcts.setPrior("attackBias");
isMcts.setConfig({iterations: 500, rolloutHorizon: 32, simDt: 0.016});
const act = isMcts.search(world, hero);
isMcts.advanceRoot(act);
isMcts.resetTree();
isMcts.lastStats;                      // {iterations, meanEss, ...}

/** InfoSetTeamMcts, team analogue. */
const isTeam = bro.ai.game.createInfoSetTeamMcts();
isTeam.setBelief(tb);
isTeam.setConfig({iterations: 400});
const joint = isTeam.search(world, [hero1, hero2]);


// =============================================================================
// Snapshots, projectiles, VecSimulation, MCTS primitives
// =============================================================================

/** Snapshot / restore, opaque handles. */
const asnap = bro.ai.game.captureAgentSnapshot(agent);
bro.ai.game.applyAgentSnapshot(agent, asnap);
asnap.id; asnap.x; asnap.z; asnap.hp; asnap.alive;

const wsnap = bro.ai.game.captureWorldSnapshot(world);
bro.ai.game.applyWorldSnapshot(world, wsnap);
wsnap.agentCount; wsnap.projectileCount; wsnap.eventCount; wsnap.nextProjectileId;
const projs = wsnap.projectiles();     // [{id, x, z, mode, ...}, ...]

/** Patch a captured WorldSnapshot with a sampled particle map (IS-MCTS
 *  determinization helper). */
bro.ai.game.patchSnapshotWithParticles(wsnap, particles);

/** Projectiles, plain objects. kind: "physical"|"magical"|"true",
 *  mode: "single"|"pierce"|"aoe" (see bro.ai.game.PROJECTILE_MODE / DAMAGE_KIND). */
const pid = bro.ai.game.spawnProjectile(world, {
  ownerId: attackerId, teamId: 0, targetId: -1,
  x: 0, z: 0, vx: 20, vz: 0, speed: 20, radius: 0.3,
  damage: 25, kind: "physical",
  remainingLife: 2.0, mode: "single",
});
const live = bro.ai.game.worldProjectiles(world);

/** VecSimulation, batched 1v1 envs for self-play training. */
const vec = bro.ai.game.createVecSimulation({
  numEnvs: 64, arenaHalfSize: 12, dt: 0.016, maxStepsPerEpisode: 600,
  hp: 100, damage: 5, attackRange: 2.5, moveSpeed: 6,
  rewardDamageDealt: 1.0, rewardKill: 100, rewardDeath: -100,
});
vec.numEnvs;
vec.seedAndReset(0x1234n);
const heroObs = vec.observe(1);           // Float32Array length N*OBS_TOTAL
const heroMask = vec.actionMask(1);       // {mask, enemyIds}
vec.applyActions(1, heroActions);         // array of N AgentAction objects
vec.applyActions(2, oppActions);
vec.step();
const d = vec.dones();                    // {done: Int32Array, winner: Int32Array}
const r = vec.rewards();                  // {hero: Float32Array, opponent: Float32Array}
vec.stepCounts(); vec.episodeCounts(); vec.resetDone(); vec.resetEnv(0);

/** MCTS primitives as first-class objects. Pass them as `evaluator`,
 *  `prior`, or `rolloutPolicy` in any Mcts / InfoSetMcts / LayeredPlanner
 *  / Commander config, in addition to the existing string presets. */
const hpEval     = bro.ai.game.createHpDeltaEvaluator();
const tHp        = bro.ai.game.createTeamHpDeltaEvaluator();
const tAdv       = bro.ai.game.createTeamAdvantageEvaluator();
const tPos       = bro.ai.game.createTeamPositionEvaluator();
const randRoll   = bro.ai.game.createRandomRollout();
const aggRoll    = bro.ai.game.createAggressiveRollout();
const scrRoll    = bro.ai.game.createScriptedRollout();
const uniformPr  = bro.ai.game.createUniformPrior();
const atkBiasPr  = bro.ai.game.createAttackBiasPrior();
const tacticPr   = bro.ai.game.createTacticPrior();
tacticPr.setMatchWeight(8.0); tacticPr.setOtherWeight(1.0);

const mctsWithNN = bro.ai.game.createMcts({
  iterations: 800, priorC: 2.0,
  evaluator: neuralEval,
  prior: neuralPrior,
  rolloutPolicy: scrRoll,
});

// String constants mirror the enum values.
bro.ai.game.PROJECTILE_MODE.Single;      // "single"
bro.ai.game.DAMAGE_KIND.Magical;         // "magical"


// =============================================================================
// Unit buffs / DoT / HoT, extended fields on agent.unit
// =============================================================================
//
// In addition to the base combat fields, every Unit proxy exposes the timed
// buff and DoT/HoT fields directly. All are read/write so abilities can
// apply effects by setting magnitude + remaining duration, and Unit.tickCooldowns
// decays them over time.
//
//   unit.armorBonus / armorBonusRemaining
//   unit.magicResistBonus / magicResistBonusRemaining
//   unit.damageMul / damageMulRemaining
//   unit.attacksMul / attacksMulRemaining
//   unit.moveSpeedMul / moveSpeedMulRemaining
//   unit.stealthChance / stealthChanceRemaining
//
//   unit.dotDps / dotRemaining / dotKind / dotSourceId
//   unit.hotRate / hotRemaining
//
//   unit.attackKind                     // "physical"|"magical"|"true"
//   unit.attackCooldown                  // read/write
//   unit.effectiveMagicResist            // armor + armorBonus
//   unit.effectiveAttacksPerSec          // attacksPerSec * attacksMul


// =============================================================================
// GenericMcts, env-agnostic PUCT search
// =============================================================================
//
// The MCTS classes above (Mcts / DecoupledMcts / TeamMcts / TacticMcts /
// OptionMcts / Commander) are welded to the bundled World/Agent/CombatAction
// model, they only plan over brogameagent's combat sim. createGenericMcts
// is the escape hatch for anything else: a custom JS-side env, a 2D platformer,
// a board game, a planner test harness. You describe the env with five
// callbacks and search() runs the same AlphaZero-style PUCT algorithm over it.
//
// Algorithm:
//
//     score(a) = Q(s, a) + cPuct · P(s, a) · √N(s) / (1 + N(s, a))
//
// Q is the action's mean discounted return; P is the prior (uniform when no
// priorFn is set); N is visit count. Leaf evaluation uses valueFn when
// provided, otherwise a random rollout of rolloutDepth steps. Optional
// Dirichlet root noise matches the AlphaZero exploration scheme.

/**
 * Create an env-agnostic PUCT MCTS searcher.
 *
 * @param {Object} opts
 * @param {Object} opts.env  - Environment description; required.
 * @param {number} opts.env.numActions     - Total action space size (max action index + 1).
 * @param {function():any}    opts.env.snapshot      - Capture current env state. Held opaquely
 *                                                     for the duration of one search() call.
 * @param {function(any)}     opts.env.restore       - Restore env from a snapshot.
 * @param {function(int):{reward:number,done:boolean}} opts.env.step - Apply action, mutate env.
 * @param {function():(Int32Array|number[])}          opts.env.legalActions - Legal action indices
 *                                                     in [0, numActions). Re-queried at every
 *                                                     expansion, so it must reflect current state.
 * @param {function():Float32Array}                   opts.env.observe - Observation vector forwarded
 *                                                     to priorFn / valueFn. Shape and semantics
 *                                                     are entirely up to you.
 * @param {number} [opts.iterations=100]   - Iterations per search() call.
 * @param {number} [opts.cPuct=1.5]        - PUCT exploration constant.
 * @param {number} [opts.gamma=0.99]       - Discount per decision step.
 * @param {number} [opts.rolloutDepth=8]   - Random-rollout depth (used only when valueFn unset).
 * @param {number} [opts.dirichletAlpha=0] - Dirichlet root noise concentration. 0 = noise off.
 * @param {number} [opts.dirichletEpsilon=0] - Mixing weight for Dirichlet noise on root prior.
 * @param {bigint|number} [opts.seed]      - Rollout / noise RNG seed.
 * @param {function(Float32Array obs, Int32Array legal):Float32Array} [opts.priorFn]
 *        - Optional learned-policy prior. Returns a probability per action over numActions.
 *          Only entries at `legal` indices are read.
 * @param {function(Float32Array obs):number} [opts.valueFn]
 *        - Optional learned-value function in [-1, 1]. When set, replaces random rollout
 *          for leaf evaluation.
 * @returns {GenericMcts}
 */
const m = bro.ai.game.createGenericMcts({
    env: {
        numActions: 6,
        snapshot()    { return mySim.snapshot(); },
        restore(s)    { mySim.restore(s); },
        step(action)  { return mySim.step(action); },     // {reward, done}
        legalActions(){ return mySim.legalActions(); },
        observe()     { return buildObs(mySim); },
    },
    iterations: 200,
    cPuct: 1.5,
    gamma: 0.99,
    rolloutDepth: 8,
    dirichletAlpha: 0.5,
    dirichletEpsilon: 0.25,
    seed: 0xC0DE1234n,
    priorFn(obs, legal) { return netForwardProbs(obs, legal); },
    valueFn(obs)        { return netForwardValue(obs); },
});

/**
 * Run a search starting from the env's current state. Returns the most-visited
 * root action, or -1 if the action space is empty. Snapshot/restore is used
 * internally; the env is left in its pre-search state on exit.
 * @returns {number}
 */
const action = m.search();

/**
 * Normalized root visit distribution over [0, numActions). Sums to 1 over
 * visited actions. Useful as the policy target for ExIt-style training.
 * @returns {Float32Array}
 */
const visits = m.rootVisits();

/**
 * Promote the subtree under `action` to the new root, reusing its statistics.
 * If `action` was never expanded, the tree is dropped and the next search()
 * rebuilds from scratch.
 */
m.advanceRoot(action);

/** Drop the search tree. */
m.reset();

/**
 * Replace the search-time configuration. Same fields as the constructor opts
 * (iterations, cPuct, gamma, rolloutDepth, dirichletAlpha, dirichletEpsilon, seed).
 */
m.setConfig({ iterations: 400, dirichletAlpha: 0 });

/** Replace or clear the prior / value functions at any time. */
m.setPriorFn((obs, legal) => netForwardProbs(obs, legal));
m.setValueFn((obs) => netForwardValue(obs));
m.setPriorFn(null);   // pass null/undefined to fall back to uniform prior

/**
 * Stats from the most recent search() call.
 *   { iterations, treeSize, bestVisits, bestAction }
 * @returns {Object}
 */
const stats = m.lastStats();


// =============================================================================
// bro.ai.game.grid, 2D grid-world / side-scrolling platformer training kit
// =============================================================================
//
// Built on top of the env-agnostic primitives (createGenericMcts,
// createPolicyValueNet, createGenericReplayBuffer, createGenericExItTrainer,
// WeightsHandle). Use this when your env is a tilemap + dynamic entities
// rather than the bundled MOBA combat sim. Each primitive is independently
// useful; the GridTrainer harness composes them into a complete loop so new
// projects don't re-author the boilerplate.

// -----------------------------------------------------------------------------
// ObsWindow, egocentric multi-channel rasterizer
// -----------------------------------------------------------------------------
//
// Caller-driven rasterization of an egocentric (cols × rows) window around
// the agent into a flat Float32Array. Tile sampler returns per-cell channel
// values; entity layers rasterize as additional channel planes by walking
// caller-supplied entity lists. No top-K cap, entities collide naturally
// into channels via accumulation (or "last write wins" in overwrite mode).
// A tail "self block" of caller-filled scalars is appended (velocities,
// flags, signed distances, whatever doesn't fit the grid).

/**
 * @param {Object} opts
 * @param {Object} opts.spec
 *   .colsBehind / .colsAhead / .rowsUp / .rowsDown, window extents
 *   .tileChannels, channels per tile cell (default 1)
 *   .selfBlockSize, tail floats appended for ego state (default 0)
 * @param {Object} [opts.tile]
 *   .channels, overrides spec.tileChannels
 *   .normalize, Float32Array per-tile-channel multiplier
 *   .oob, Float32Array filled into out-of-world cells
 *   .sample(col, row) → bool | number | Array | Float32Array
 *     Return false for OOB cells; otherwise the rasterizer writes either a
 *     bool/number broadcast across all channels, or an array of length
 *     tileChannels.
 * @param {Array<Object>} [opts.layers]: entity layers in z-order
 *   .channels, per-cell channels for this layer
 *   .overwrite, true => last write wins; false (default) => additive
 *   .normalize, per-channel multiplier applied AFTER accumulation
 *   .enumerate() → number, count of entities to walk
 *   .sample(i)   → { col, row, values }, values is per-channel array; or
 *                                          { col, row, value } for a
 *                                          single-channel layer shorthand
 * @returns {ObsWindow}
 */
const obsWin = bro.ai.game.grid.createObsWindow({
    spec: { colsBehind: 4, colsAhead: 11, rowsUp: 6, rowsDown: 6,
            tileChannels: 2, selfBlockSize: 8 },
    tile: {
        normalize: new Float32Array([1, 1]),
        sample(c, r) {
            const t = world.getTile(c, r);
            if (!t) return false;
            return [t.solid ? 1 : 0, t.hazard ? 1 : 0];
        },
    },
    layers: [{
        channels: 3,
        normalize: new Float32Array([1, 0.01, 0.5]),
        enumerate() { return mobs.length; },
        sample(i) {
            const m = mobs[i];
            return { col: m.col, row: m.row, values: [1, m.hp, m.kind] };
        },
    }],
});

obsWin.outDim;          // total Float32 length
obsWin.layout();        // {cols, rows, tileOffset, tileSize, layers:[{offset,channels,size}], selfOffset, selfSize, total}
const buf = obsWin.build(egoCol, egoRow,
    new Float32Array([vx, vy, jumping ? 1 : 0, /* ... */]));   // returns Float32Array of length outDim


// -----------------------------------------------------------------------------
// FrameStack, last-k frames concatenated for temporal context
// -----------------------------------------------------------------------------
//
// Stateful ring of k inner observations. read() returns the chronological
// concatenation [oldest, ..., newest]. reset() at episode boundaries,
// without it, the first decision sees the previous episode's tail.

/** @param {{innerDim, k}} opts @returns {FrameStack} */
const stack = bro.ai.game.grid.createFrameStack({ innerDim: obsWin.outDim, k: 4 });
stack.outDim;       // = innerDim * k
stack.filled;       // count of pushed frames since last reset (clamped at k)
stack.push(buf);
const stacked = stack.read();   // Float32Array length k * innerDim
stack.reset();      // call at episode start


// -----------------------------------------------------------------------------
// FailureTape, penalize repeated bad-state actions
// -----------------------------------------------------------------------------
//
// Records (signature, action) pairs from the tail of failed episodes.
// Emits floor-clamped penalty^count multipliers for use as a prior post-
// multiplier in GenericMcts.priorFn, discourages repeating mistakes at
// known-bad state signatures.

/** @param {{tapeDepth, ringCapacity, penalty, floor}} opts */
const tape = bro.ai.game.grid.createFailureTape({
    tapeDepth: 16,
    ringCapacity: 4096,
    penalty: 0.5,
    floor: 0.05,
});
tape.recordFailure([
    { sig: "5|hazard", action: 1 },
    { sig: "6|hazard", action: 1 },
    // ... last tapeDepth steps of the failed episode
]);
const m1 = tape.multipliers("5|hazard", /*numActions*/ 6);   // Float32Array
const adjusted = tape.applyPriors("5|hazard", netPrior);     // returns adjusted array
tape.size; tape.capacity; tape.clear();


// -----------------------------------------------------------------------------
// BestCrop, ranked snapshot pool for "search from historical bests"
// -----------------------------------------------------------------------------
//
// Bounded pool of (snapshot, action prefix, score, depth). Effective rank =
// score + depthBonus*depth - ageDecay*age. seed() picks uniformly from the
// top-K, feed its result to GenericMcts to start search at a historical
// good state instead of always replaying from spawn.

/** @param {{capacity, depthBonus, ageDecay, seedTopK, seed}} opts */
const crop = bro.ai.game.grid.createBestCrop({
    capacity: 64, depthBonus: 0.01, ageDecay: 0.001, seedTopK: 4,
    seed: 0xC0DE1234n,
});
crop.push({ snapshot: env.snapshot(), prefix: actionsTaken,
            score: episodeReturn, depth: ep_depth });
const pickedSeed = crop.seed();         // { snapshot, prefix }
if (pickedSeed.snapshot) {
    env.restore(pickedSeed.snapshot);
    for (const a of pickedSeed.prefix) env.step(a);
    const action = mcts.search();        // search from a deep good state
}
crop.size; crop.capacity; crop.clear();


// -----------------------------------------------------------------------------
// PotentialShaper / StallDetector, reward shaping helpers
// -----------------------------------------------------------------------------

/** Ng/Harada/Russell potential-based shaping: emit γΦ(s')-Φ(s) per step.
 *  Preserves the optimal policy of the underlying MDP. */
const shaper = bro.ai.game.grid.createPotentialShaper({ gamma: 0.99 });
shaper.reset(/*phi0*/ -distToGoal(state) * 0.01);
const bonus = shaper.step(/*phi'*/ -distToGoal(nextState) * 0.01);
// r_shaped = r + bonus

/** Stall detector, fires once a sliding window of progress samples has
 *  spread < epsilon. Use for early termination of stuck episodes. */
const stallDet = bro.ai.game.grid.createStallDetector({ epsilon: 0.5, patience: 60 });
stallDet.reset();
if (stallDet.tick(progress(state))) endEpisode("stalled");


// -----------------------------------------------------------------------------
// generateBC, heuristic policy → GenericSituation tuples
// -----------------------------------------------------------------------------
//
// Run a hand-coded policy from each starting snapshot, filter by minReturn,
// emit GenericSituations with one-hot policy targets and per-step return-to-go
// as the value target. Use to escape cold-start traps where a freshly-
// initialized net never stumbles into reward.

/** @param {Object} opts
 *  @param {Object} opts.env: same shape as createGenericMcts env (numActions, snapshot, restore, step, legalActions, observe)
 *  @param {function(obs, legal): number} opts.heuristic
 *  @param {Array<any>} opts.starts: env-snapshot objects
 *  @param {number} [opts.minReturn=0]
 *  @param {number} [opts.rolloutHorizon=256]
 *  @param {number} [opts.gamma=0.99]
 *  @param {boolean} [opts.clipValue=true]
 *  @returns {Array<GenericSituation>}
 */
const bcSits = bro.ai.game.grid.generateBC({
    env: { numActions, snapshot, restore, step, legalActions, observe },
    heuristic: (obs, legal) => pickGreedyAction(obs, legal),
    starts: [snap1, snap2, snap3],
    minReturn: 0.5, rolloutHorizon: 200, gamma: 0.99,
});
gtrainer.warmupWith(bcSits);


// -----------------------------------------------------------------------------
// GenericRecorder / GenericReplayReader, schema-driven .bgargrid replays
// -----------------------------------------------------------------------------
//
// Caller declares roster / frame / event schemas at open time (i32, i64, f32,
// f64). Recorder streams variable-count rows per frame; reader random-accesses
// any frame via the offset index appended at close. Distinct magic from .bgar
// so the existing combat-shaped reader isn't burdened.

const rec = bro.ai.game.grid.createGenericRecorder();
rec.open("ep_0001.bgargrid", /*episodeId*/ 1n, /*seed*/ 0xC0DEn, /*dt*/ 1/60, {
    roster: [{name: "id", type: "i32"}, {name: "kind", type: "i32"}],
    frame:  [{name: "id", type: "i32"}, {name: "x", type: "f32"}, {name: "y", type: "f32"}],
    events: [{name: "src", type: "i32"}, {name: "amount", type: "f32"}],
});
rec.writeRoster([[0, 1], [1, 2]]);             // rows are arrays in schema order
rec.recordFrame(0n, 0.0,
    [[0, 1.0, 2.0], [1, 3.0, 4.0]],            // entities this frame
    [[1, 12.0]]);                               // events this frame
rec.recordFrame(1n, 1/60, [[0, 1.5, 2.5]], []);
rec.frameCount;
rec.close();

const rr = bro.ai.game.grid.createGenericReplayReader();
rr.open("ep_0001.bgargrid");
rr.frameCount;
const fr = rr.frame(0);                         // { stepIdx, elapsed, rows, events }
const xs = rr.trajectory(/*rowIndex*/ 0, "x"); // values for that field across all frames


// -----------------------------------------------------------------------------
// GridTrainer, owns net + buffer + trainer + best/failure trackers
// -----------------------------------------------------------------------------
//
// Wires the generic primitives into a complete training loop. Producers (self-
// play searches) call ingestSituation / ingestEpisode from any thread; the
// harness drains lock-free MPSC rings, runs SGD on the PolicyValueNet,
// publishes weights via a WeightsHandle, optionally writes a checkpoint ring
// + best.bin to disk, and emits weightsUpdated / bestRotated / episodeIngested
// events you can read with pollEvents() to drive UI / telemetry.
//
// Two modes:
//   - Async: start() spins a trainer thread; producers ingest from anywhere.
//             stop() joins.
//   - Sync:  stepSync(n) drains the rings and runs n SGD steps in line.

/** @param {Object} opts
 *  @param {Object} opts.net: { inDim, hidden:[...], valueHidden, numActions, seed:bigint }
 *  @param {Object} [opts.buffer]: { capacity }
 *  @param {Object} [opts.trainer]: { lr, momentum, batch, policyWeight, valueWeight, publishEvery, rngSeed }
 *  @param {Object} [opts.ckpt]: { dir, ringSize, bestWindow }   omit for no disk writes
 *  @param {number} [opts.ingestBurst=64]
 *  @param {number} [opts.stepsPerTick=1]
 *  @returns {GridTrainer}
 */
const gtrainer = bro.ai.game.grid.createGridTrainer({
    net:     { inDim: stack.outDim, hidden: [128, 128], valueHidden: 64, numActions: 6, seed: 1n },
    buffer:  { capacity: 65536 },
    trainer: { lr: 1e-3, momentum: 0.9, batch: 64,
               policyWeight: 1.0, valueWeight: 1.0, publishEvery: 200 },
    ckpt:    { dir: "ckpts/", ringSize: 8, bestWindow: 50 },
});

gtrainer.warmupWith(bcSits);                    // optional BC warmup

// Producers, safe from any thread.
gtrainer.ingestSituation({
    obs:          stack.read(),
    policyTarget: mcts.rootVisits(),
    actionMask:   maskForLegal,
    valueTarget:  clippedReturn,
});
gtrainer.ingestEpisode({
    totalReturn: r,
    depth:       t,
    failed:      r <= 0,
    snapshot:    episodeStartSnapshot,          // optional → BestCrop on success
    prefix:      actionsTaken,                   // optional replay path
    tail:        [/* {sig, action} ... */],     // optional → FailureTape on failure
});

gtrainer.start();                               // async mode
// ... or
gtrainer.stepSync(/*sgdSteps*/ 16);             // sync mode

for (const ev of gtrainer.pollEvents()) {
    if (ev.kind === "weightsUpdated")  notifySearchers(ev.version);
    if (ev.kind === "bestRotated")     ui.markCheckpoint(ev.path, ev.meanReturn);
    if (ev.kind === "episodeIngested") metrics.push(ev.meanReturn);
}

gtrainer.stats();
//   { totalSteps, totalPublishes, episodesIngested,
//     trailingMeanReturn, bestMeanReturn, bufferSize, running }

gtrainer.stop();
