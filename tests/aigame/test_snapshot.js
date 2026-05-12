// Snapshot capture/apply for agent and world.

const G = bro.ai.game;

const w = G.createWorld();
const a = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, x: 3, z: 4 });
const b = G.createAgent({ id: 2, teamId: 1, hp: 100, damage: 10, x: -2, z: 0 });
w.addAgent(a);
w.addAgent(b);

// Agent snapshot.
const asnap = G.captureAgentSnapshot(a);
assert(asnap && typeof asnap === 'object', 'agent snapshot is object');
assert(asnap.id === 1, 'snapshot.id=1, got ' + asnap.id);
assert(Math.abs(asnap.x - 3) < 1e-4, 'snapshot.x=3, got ' + asnap.x);
assert(Math.abs(asnap.z - 4) < 1e-4, 'snapshot.z=4, got ' + asnap.z);
assert(asnap.hp === 100, 'snapshot.hp=100, got ' + asnap.hp);
assert(asnap.alive === true, 'snapshot.alive=true');

// Mutate the agent.
a.setPosition(99, 99);
a.unit.hp = 1;

// Restore.
G.applyAgentSnapshot(a, asnap);
assert(Math.abs(a.x - 3) < 1e-4, 'restored x=3, got ' + a.x);
assert(Math.abs(a.z - 4) < 1e-4, 'restored z=4, got ' + a.z);
assert(Math.abs(a.unit.hp - 100) < 1e-4, 'restored hp=100, got ' + a.unit.hp);

// World snapshot.
const wsnap = G.captureWorldSnapshot(w);
assert(wsnap && typeof wsnap === 'object', 'world snapshot is object');
assert(wsnap.agentCount === 2, 'wsnap.agentCount=2, got ' + wsnap.agentCount);

// Mutate world: move + damage.
a.setPosition(10, 10);
a.unit.hp = 5;
b.unit.hp = 5;

// Restore world.
G.applyWorldSnapshot(w, wsnap);
assert(Math.abs(a.x - 3) < 1e-3 && Math.abs(a.z - 4) < 1e-3,
    'world restore: a position back to (3,4), got (' + a.x + ',' + a.z + ')');
assert(Math.abs(a.unit.hp - 100) < 1e-3, 'world restore: a.hp=100, got ' + a.unit.hp);
assert(Math.abs(b.unit.hp - 100) < 1e-3, 'world restore: b.hp=100, got ' + b.unit.hp);

console.log('test_snapshot: OK');
