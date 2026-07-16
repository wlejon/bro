// NavGrid creation, A* pathfinding, dynamic obstacles.

const G = bro.ai.game;

// Open grid: no obstacles.
const navOpen = G.createNavGrid({ minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5 });
assert(navOpen.isWalkable(0, 0), 'open: origin walkable');
assert(navOpen.isWalkable(10, 5), 'open: arbitrary cell walkable');

const pathOpen = navOpen.findPath(-10, 0, 10, 0);
assert(Array.isArray(pathOpen), 'open path is array');
assert(pathOpen.length >= 2, 'open path has at least start+end, got ' + pathOpen.length);
const first = pathOpen[0], last = pathOpen[pathOpen.length - 1];
assert(Math.hypot(first.x - (-10), first.z - 0) < 1.5, 'open path starts near start');
assert(Math.hypot(last.x - 10, last.z - 0) < 1.5, 'open path ends near goal');

// Grid with central obstacle.
const nav = G.createNavGrid({
    minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5,
    obstacles: [{ x: 0, z: 0, hw: 2, hd: 2 }], padding: 0.4,
});
assert(!nav.isWalkable(0, 0), 'obstacle: origin blocked');
assert(nav.isWalkable(10, 10), 'obstacle: open cell walkable');

const path = nav.findPath(-10, 0, 10, 0);
assert(path.length >= 2, 'obstacle path nonempty, got ' + path.length);

// Path length should be > straight-line distance (must route around).
let pathLen = 0;
for (let i = 1; i < path.length; i++) {
    pathLen += Math.hypot(path[i].x - path[i-1].x, path[i].z - path[i-1].z);
}
assert(pathLen > 20, 'obstacle path longer than straight-line (20), got ' + pathLen.toFixed(2));
assert(pathLen < 50, 'obstacle path is not absurdly long, got ' + pathLen.toFixed(2));

// No waypoint should lie inside the obstacle.
for (const wp of path) {
    const inside = Math.abs(wp.x) < 2 && Math.abs(wp.z) < 2;
    assert(!inside, 'no waypoint inside obstacle: got (' + wp.x + ',' + wp.z + ')');
}

// Unreachable: walled off goal.
const walled = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    obstacles: [
        { x: 5, z: 0,  hw: 0.3, hd: 6 },  // vertical wall slicing right side
    ],
});
// Try to go from left of wall to right of wall; should still find a path around
// (wall doesn't reach edges). Pick a true unreachable: enclose target.
const enclosed = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    obstacles: [
        { x: 5,  z: 5,  hw: 1.5, hd: 0.3 }, // top
        { x: 5,  z: 8,  hw: 1.5, hd: 0.3 }, // bottom
        { x: 3.5, z: 6.5, hw: 0.3, hd: 1.5 }, // left
        { x: 6.5, z: 6.5, hw: 0.3, hd: 1.5 }, // right
    ],
    padding: 0.4,
});
const noPath = enclosed.findPath(0, 0, 5, 6.5);
assert(noPath.length === 0, 'unreachable returns empty path, got len=' + noPath.length);

// Dynamic obstacle invalidates a route.
const dyn = G.createNavGrid({ minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5 });
const p1 = dyn.findPath(-10, 0, 10, 0);
assert(p1.length >= 2, 'dyn before-add: path exists');
let p1len = 0;
for (let i = 1; i < p1.length; i++) p1len += Math.hypot(p1[i].x - p1[i-1].x, p1[i].z - p1[i-1].z);

dyn.addObstacle({ x: 0, z: 0, hw: 3, hd: 3 }, 0.4);
assert(!dyn.isWalkable(0, 0), 'dyn after-add: origin blocked');
const p2 = dyn.findPath(-10, 0, 10, 0);
assert(p2.length >= 2, 'dyn after-add: path exists');
let p2len = 0;
for (let i = 1; i < p2.length; i++) p2len += Math.hypot(p2[i].x - p2[i-1].x, p2[i].z - p2[i-1].z);
assert(p2len > p1len + 0.5, 'dyn: path got longer after obstacle added (' + p1len.toFixed(2) + ' -> ' + p2len.toFixed(2) + ')');

console.log('test_navgrid: OK');
