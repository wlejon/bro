// The AI & Navigation probe: NavGrid, NavMesh, and AIAgent integration in bronze_host.
//
// Tests:
// - AI, AINavGrid, AINavMesh, AIAgent globals
// - NavGrid creation, bounds, isWalkable, setWalkable, setCellCost, addObstacle, findPath, LOS
// - NavMesh baking from triangle soup, valid getter, closestPoint, randomPoint, raycast, findPath
// - AIAgent creation with NavMesh, position, velocity, speed, radius, setGoal, update, stop, destroy
// - AI.computeAim utility

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// ---------------------------------------------------------------------------
// 1. Global existence
// ---------------------------------------------------------------------------

say('global.hasAI', typeof AI === 'object');
say('global.hasAINavGrid', typeof AINavGrid === 'function');
say('global.hasAINavMesh', typeof AINavMesh === 'function');
say('global.hasAIAgent', typeof AIAgent === 'function');

// ---------------------------------------------------------------------------
// 2. NavGrid Creation, Obstacles & Pathfinding
// ---------------------------------------------------------------------------

const grid = AI.createNavGrid({
    minX: 0,
    minZ: 0,
    maxX: 20,
    maxZ: 20,
    cellSize: 1.0
});

say('navgrid.created', grid !== null);
say('navgrid.width', grid.width);
say('navgrid.height', grid.height);
say('navgrid.isWalkable_center', grid.isWalkable(10, 10));

// Add obstacle at (10, 10)
grid.addObstacle({ cx: 10, cz: 10, hw: 2, hd: 2 });
say('navgrid.isWalkable_blocked', grid.isWalkable(10, 10));

// Test setWalkable
grid.setWalkable(10, 10, true);
say('navgrid.setWalkable', grid.isWalkable(10, 10));

// Test setCellCost
grid.setCellCost(10, 10, -1);
say('navgrid.setCellCost_blocked', grid.isWalkable(10, 10));

// Re-block obstacle box at (10, 10)
grid.addObstacle({ cx: 10, cz: 10, hw: 2, hd: 2 });

// Test findPath
const gridPath = grid.findPath(2, 2, 18, 18);
say('navgrid.pathFound', Array.isArray(gridPath) && gridPath.length >= 2);
say('navgrid.pathPartial', gridPath.partial === false);

// Line of Sight
say('navgrid.los_blocked', AI.hasLineOfSight(2, 2, 18, 18, grid));
say('navgrid.los_clear', AI.hasLineOfSight(2, 2, 2, 18, grid));

// ---------------------------------------------------------------------------
// 3. NavMesh Baking & Queries
// ---------------------------------------------------------------------------

// Simple ground plane (20x20) in XZ
const verts = new Float32Array([
    0, 0, 0,
    20, 0, 0,
    20, 0, 20,
    0, 0, 20
]);
const indices = new Uint32Array([
    0, 2, 1,
    0, 3, 2
]);

const navMesh = AI.bakeNavMesh({
    vertices: verts,
    indices: indices,
    cellSize: 0.25,
    cellHeight: 0.2,
    agentRadius: 0.5,
    agentHeight: 2.0,
    agentMaxClimb: 0.4,
    agentMaxSlope: 45
});

say('navmesh.valid', navMesh.valid);

// Closest Point
const cp = navMesh.closestPoint({ x: 10, y: 0.5, z: 10 });
say('navmesh.closestPoint_valid', cp !== null);
say('navmesh.closestPoint_x', Math.round(cp.x));
say('navmesh.closestPoint_y', Math.round(cp.y));
say('navmesh.closestPoint_z', Math.round(cp.z));

// Random Point
const rp = navMesh.findRandomPoint(42);
say('navmesh.randomPoint_valid', rp !== null && rp.x >= 0 && rp.x <= 20 && rp.z >= 0 && rp.z <= 20);

// Raycast
const rcClear = navMesh.raycast({ x: 2, y: 0, z: 2 }, { x: 18, y: 0, z: 18 });
say('navmesh.raycast_clear', rcClear.hit === false);

const rcEdge = navMesh.raycast({ x: 10, y: 0, z: 10 }, { x: 50, y: 0, z: 10 });
say('navmesh.raycast_edge_hit', rcEdge.hit === true);

// Pathfinding
const nmPath = navMesh.findPath({ x: 2, y: 0, z: 2 }, { x: 18, y: 0, z: 18 });
say('navmesh.path_valid', Array.isArray(nmPath) && nmPath.length >= 2);
say('navmesh.path_partial', nmPath.partial === false);

// ---------------------------------------------------------------------------
// 4. AIAgent Creation & Movement
// ---------------------------------------------------------------------------

const agent = AI.createAgent({
    position: { x: 2, y: 0, z: 2 },
    radius: 0.5,
    maxSpeed: 8.0,
    maxAcceleration: 20.0,
    navMesh: navMesh
});

say('agent.pos_x', Math.round(agent.position.x));
say('agent.pos_z', Math.round(agent.position.z));
say('agent.speed', agent.maxSpeed);
say('agent.radius', agent.radius);

// Set Goal and update
agent.setGoal(18, 0, 18);
say('agent.hasTarget', agent.hasTarget);

// Step forward
agent.update(0.5);
say('agent.moved', agent.position.x > 2 && agent.position.z > 2);

// Stop and Destroy
agent.stop();
say('agent.hasTarget_after_stop', agent.hasTarget);

agent.destroy();
say('agent.destroyed', true);

// ---------------------------------------------------------------------------
// 5. Aim Utilities
// ---------------------------------------------------------------------------

const aimDirect = AI.computeAim({ x: 0, y: 1.6, z: 0 }, { x: 0, y: 1.6, z: -10 });
say('aim.valid', aimDirect.valid);
say('aim.yaw', Math.round(aimDirect.yaw));
say('aim.pitch', Math.round(aimDirect.pitch));
