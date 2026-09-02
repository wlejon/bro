// bro.ai.game from COMPILED code (src/bronze_host/host_ai_game.cpp): the
// namespace the interpreted bindings publish (docs/ai-game-api.js), over the
// same brogameagent objects the `AI` global wraps. What only this check
// catches: a compiled program that binds `bro.ai.game` at module load and
// finds no namespace, a HexNav whose typed arrays cross as copies rather
// than views, and an ORCA world whose agents never actually get stepped.
//
// Every line is `APP <name>=<value>`, every expectation derived from the
// doc before the first run: the hex table below is hand-authored, its paths
// are the unique cheapest routes, and the world's outcomes are the ones the
// doc promises ("they pass, not overlap").

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// A typed array's cells, comma-joined, by an index walk — what the check is
// about is the cells, not which realm's `join` a view answers to.
function list(view) {
    const out = [];
    for (let i = 0; i < view.length; i++) out.push(view[i]);
    return out.join(',');
}

const game = bro.ai.game;
say('namespace', typeof game);
say('createHexNav', typeof game.createHexNav);
say('createWorld', typeof game.createWorld);
say('createAgent', typeof game.createAgent);
say('navMeshAvailable', game.navMeshAvailable);

// --- HexNav ------------------------------------------------------------------
//
// A 4x4 odd-r grid, idx = y*4 + x, slot c*6+d = the cost of entering c from
// direction d (0=E 1=NE 2=NW 3=W 4=SW 5=SE). Row 0 is even (not shoved), so
// from (x,0) SE is (x,1); row 1 is odd, so from (x,1) NE is (x+1,0).

const SIZE = 4;
const hex = game.createHexNav({ size: SIZE });
say('hexClass', hex instanceof AIHexNav);
say('hexSize', hex.size);

const table = new Float64Array(SIZE * SIZE * 6);
table.fill(1);
// Cell 2 = (2,0) cannot be entered from anywhere; cell 4 = (0,1) costs 5.
for (let d = 0; d < 6; d++) table[2 * 6 + d] = Infinity;
for (let d = 0; d < 6; d++) table[4 * 6 + d] = 5;
say('setStepCosts', hex.setStepCosts('t', table));
say('hasStepCosts', hex.hasStepCosts('t') + ',' + hex.hasStepCosts('nope'));

// (0,0) -> (3,0) with (2,0) walled: the 4-step routes are E,SE,E,NE through
// cells 1,5,6 (cost 4) and SE,E,E,NE through 4,5,6 (cost 8, because of cell
// 4). Only one is cheapest.
const route = hex.findPath('t', 0, 0, 3, 0);
say('routeType', Object.prototype.toString.call(route));
say('route', list(route));
// The same route at a budget it cannot meet is no route.
say('routeOverBudget', hex.findPath('t', 0, 0, 3, 0, 3));
// An unknown table is not a crash.
say('routeUnknownTable', hex.findPath('missing', 0, 0, 3, 0));

// Dijkstra out from (0,0) to cost 1: cell 1 is reached at 1 through 0,
// cell 2 is a wall, cell 3 is past the budget.
const wash = hex.movementField('t', 0, 0, 1);
say('washCost', wash.cost[0] + ',' + wash.cost[1] + ',' + wash.cost[2] + ',' + wash.cost[3]);
say('washParent', wash.parent[0] + ',' + wash.parent[1]);
say('washCostType', Object.prototype.toString.call(wash.cost));

// Breach the wall in place: the six entry slots of cell 2 become 1, and the
// straight row is the cheapest route again (cost 3, uniquely: three E steps
// is the only 3-step path between two cells three apart on one row).
say('updateStepCosts', hex.updateStepCosts('t', new Int32Array([2]), new Float64Array([1, 1, 1, 1, 1, 1])));
say('routeAfterBreach', list(hex.findPath('t', 0, 0, 3, 0)));

// Components: one label per cell, and two cells a path joins share one.
const comp = hex.components('t');
say('compLength', comp.length);
say('compJoined', comp[0] === comp[3]);

// A clearance table gates the footprint search: (1,0) cannot be stood on and
// (2,0) is crushing (its step doubles), so the clearance route from (0,0) to
// (3,0) has to leave through cell 4 (the only other neighbour, cost 5) and
// then the cheapest way on is E,E,NE through 5,6 (5+1+1+1 = 8) — the
// alternative E,NE,E through 5,2 pays 5+1+2+1 = 9. The plain route is
// unaffected.
const clr = new Uint8Array(SIZE * SIZE);
clr.fill(1);
clr[1] = 3;
clr[2] = 2;
say('setClearance', hex.setClearance('c', clr));
say('hasClearance', hex.hasClearance('c'));
say('routeRadius', list(hex.findPathRadius('t', 'c', 0, 0, 3, 0)));
say('routePlainStill', list(hex.findPath('t', 0, 0, 3, 0)));

// A size mismatch is the doc's RangeError, not a silent install.
let mismatch = 'none';
try {
    hex.setStepCosts('bad', new Float64Array(6));
} catch (e) {
    mismatch = e instanceof RangeError;
}
say('sizeMismatch', mismatch);

// --- World: two agents crossing under ORCA -----------------------------------

const world = game.createWorld();
say('worldClass', world instanceof AIWorld);
say('avoidanceDefault', world.avoidanceEnabled);
world.setAvoidance(true);
say('avoidanceOn', world.avoidanceEnabled);

function runCrossing(avoid) {
    const w = game.createWorld();
    w.setAvoidance(avoid);
    const a = game.createAgent({ x: -5, z: 0, speed: 2, radius: 0.5, avoidance: avoid });
    const b = game.createAgent({ x: 5, z: 0, speed: 2, radius: 0.5, avoidance: avoid });
    w.addAgent(a);
    w.addAgent(b);
    // Routes handed over as plain arrays — the shape a game's own pathfinder
    // produces — one of objects, one of pairs.
    a.setPath([{ x: 5, z: 0 }]);
    b.setPath([[-5, 0]]);
    let minSep = Infinity;
    for (let i = 0; i < 600; i++) {
        w.tick(1 / 60);
        const dx = a.x - b.x;
        const dz = a.z - b.z;
        const sep = Math.sqrt(dx * dx + dz * dz);
        if (sep < minSep) minSep = sep;
    }
    return { w: w, a: a, b: b, minSep: minSep };
}

// Without avoidance the two walk straight through each other: same speed,
// mirrored start, so they meet head-on at the origin.
const plain = runCrossing(false);
say('plainOverlap', plain.minSep < 0.5);
say('plainArrived', plain.a.x > 4 && plain.b.x < -4);

// With it, they pass: never closer than the sum of their radii, and still
// both across after ten seconds at two units a second over ten units.
const orca = runCrossing(true);
say('orcaNoOverlap', orca.minSep >= 0.9);
say('orcaArrived', orca.a.x > 4 && orca.b.x < -4);
say('orcaAgentCount', orca.w.agentCount);
orca.w.removeAgent(orca.a);
say('orcaAgentCountAfterRemove', orca.w.agentCount);

// --- Route bookkeeping on one agent ------------------------------------------

const solo = game.createAgent({ x: 0, z: 0, speed: 1, radius: 0.4 });
solo.setPath([{ x: 3, z: 0 }, { x: 3, z: 3 }]);
say('pathLength', solo.path.length);
say('pathFirst', solo.path[0].x + ',' + solo.path[0].z);
say('waypointStart', solo.currentWaypoint);
say('hasTarget', solo.hasTarget);
world.addAgent(solo);
world.tick(1 / 60);
// One step east: the velocity points along +x. Not EXACTLY along it — the
// avoidance pass keeps a symmetry-breaking nudge of a few thousandths even
// with nobody to avoid — so the pin is the direction, not the zero.
say('velocityEast', solo.velocity.x > 0 && Math.abs(solo.velocity.z) < 0.01 * solo.velocity.x);
say('movedEast', solo.x > 0);
solo.clearTarget();
say('clearedTarget', solo.hasTarget);
say('clearedPath', solo.path.length);

// A route that is not an array is refused, not walked.
let badPath = 'none';
try {
    solo.setPath({ x: 1, z: 2 });
} catch (e) {
    badPath = e instanceof TypeError;
}
say('setPathRefusal', badPath);

say('done', 'true');
