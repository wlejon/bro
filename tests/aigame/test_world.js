// World creation, addAgent, get/set, tick.

const G = bro.ai.game;

const w = G.createWorld();
assert(w, 'world created');
assert(typeof w.tick === 'function', 'world.tick exists');
assert(typeof w.addAgent === 'function', 'world.addAgent exists');

const a = G.createAgent({ x: 1, z: 2, speed: 5, hp: 100, damage: 10, attackRange: 3,
                          id: 1, teamId: 0 });
const b = G.createAgent({ x: 5, z: 2, speed: 5, hp: 100, damage: 10, attackRange: 3,
                          id: 2, teamId: 1 });

w.addAgent(a);
w.addAgent(b);

assert(w.agentCount === 2, 'agentCount=2 after add, got ' + w.agentCount);

// Unit state.
assert(a.unit.hp === 100, 'unit.hp=100');
assert(a.unit.maxHp === 100, 'unit.maxHp=100');
assert(a.unit.id === 1, 'unit.id=1');
assert(a.unit.teamId === 0, 'unit.teamId=0');
assert(a.unit.alive === true, 'alive=true');

// Set/get position via setPosition.
a.setPosition(7, 8);
assert(Math.abs(a.x - 7) < 1e-5 && Math.abs(a.z - 8) < 1e-5,
    'setPosition reflected in x/z, got (' + a.x + ',' + b.z + ')');

// Tick advances time without crash.
w.tick(1/60);
w.tick(1/60);

// Apply damage and verify HP decreases.
const dmg = w.dealDamage ? null : null;  // dealDamage may exist via world; use takeDamage on unit instead.
const before = a.unit.hp;
a.unit.takeDamage(25, 'physical');
const after = a.unit.hp;
assert(after < before, 'hp decreases after takeDamage: ' + before + ' -> ' + after);

// alive flips when hp <= 0.
a.unit.takeDamage(200, 'true');
assert(a.unit.alive === false, 'alive=false when hp depleted, hp=' + a.unit.hp);

// Cooldowns tick.
a.unit.attackCooldown = 1.0;
a.unit.tickCooldowns(0.5);
assert(Math.abs(a.unit.attackCooldown - 0.5) < 1e-4,
    'cooldown decays via tickCooldowns: ' + a.unit.attackCooldown);

// nearestEnemy: a is dead, b alone should yield null for b's nearest enemy
const w2 = G.createWorld();
const h1 = G.createAgent({ id: 10, teamId: 0, hp: 100, x: 0, z: 0 });
const h2 = G.createAgent({ id: 11, teamId: 1, hp: 100, x: 5, z: 0 });
w2.addAgent(h1);
w2.addAgent(h2);
const enemy = w2.nearestEnemy(h1);
assert(enemy, 'h1 finds an enemy');
assert(enemy.unit.id === 11, 'enemy is h2, got id=' + (enemy && enemy.unit && enemy.unit.id));

// Events stream (should be array).
const evs = w2.events;
assert(Array.isArray(evs), 'world.events is array');

console.log('test_world: OK');
