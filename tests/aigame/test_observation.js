// buildObservation / buildActionMask / createRewardTracker.

const G = bro.ai.game;

const w = G.createWorld();
const h = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, attackRange: 3, x: 0, z: 0 });
const e = G.createAgent({ id: 2, teamId: 1, hp: 100, damage: 10, attackRange: 3, x: 5, z: 0 });
const ally = G.createAgent({ id: 3, teamId: 0, hp: 100, damage: 10, attackRange: 3, x: -2, z: 0 });
[h, e, ally].forEach(a => w.addAgent(a));

// Observation.
const obs = G.buildObservation(h, w);
assert(obs instanceof Float32Array, 'observation is Float32Array');
assert(typeof G.OBS_TOTAL === 'number', 'OBS_TOTAL exposed, got ' + typeof G.OBS_TOTAL);
assert(obs.length === G.OBS_TOTAL,
    'obs length == OBS_TOTAL, got ' + obs.length + ' vs ' + G.OBS_TOTAL);

// No NaNs.
for (let i = 0; i < obs.length; i++) {
    assert(Number.isFinite(obs[i]), 'obs[' + i + '] finite, got ' + obs[i]);
}

// Action mask.
const am = G.buildActionMask(h, w);
assert(am && 'mask' in am && 'enemyIds' in am, 'mask returns {mask, enemyIds}');
assert(am.mask instanceof Float32Array, 'mask is Float32Array');
assert(am.enemyIds instanceof Int32Array, 'enemyIds is Int32Array');
assert(typeof G.MASK_TOTAL === 'number', 'MASK_TOTAL constant');
assert(am.mask.length === G.MASK_TOTAL, 'mask len = MASK_TOTAL, got ' + am.mask.length);

// All mask entries in [0, 1].
for (let i = 0; i < am.mask.length; i++) {
    assert(am.mask[i] === 0 || am.mask[i] === 1 ||
           (am.mask[i] >= 0 && am.mask[i] <= 1),
        'mask[' + i + '] in [0,1], got ' + am.mask[i]);
}

// Enemy slot 0 should reference enemy e (id 2) since e is the only enemy.
let foundEnemy = false;
for (let i = 0; i < am.enemyIds.length; i++) {
    if (am.enemyIds[i] === 2) foundEnemy = true;
}
assert(foundEnemy, 'enemyIds includes enemy id 2, got [' + Array.from(am.enemyIds).join(',') + ']');

// Reward tracker: damage taken should be reported on consume.
const tracker = G.createRewardTracker(h, w);
const before = tracker.consume(h, w);
assert(before && typeof before === 'object', 'tracker.consume returns object');
assert('damageDealt' in before && 'damageTaken' in before &&
       'kills' in before && 'deaths' in before && 'distanceTravelled' in before,
       'reward delta shape: ' + JSON.stringify(before));
// Initial consume should be zero deltas.
assert(before.damageTaken === 0 && before.damageDealt === 0,
    'initial deltas zero, got ' + JSON.stringify(before));

// Apply damage to h via world; need world.dealDamage(attacker, target, amount).
// We'll use the agent's takeDamage and then check tracker — but tracker reads
// world.events(), so we need to use world's damage path.
const dmgDealt = w.dealDamage(e, h, 25, 'physical');
assert(typeof dmgDealt === 'number', 'world.dealDamage returns number, got ' + typeof dmgDealt);
assert(dmgDealt > 0, 'damage dealt > 0, got ' + dmgDealt);

const delta = tracker.consume(h, w);
if (delta.damageTaken <= 0) {

    assert(false, 'BUG: reward tracker damageTaken=' + delta.damageTaken +
           ' after dealing ' + dmgDealt + ' damage to h');
}
assert(delta.damageTaken > 0, 'tracker reports damage taken, got ' + delta.damageTaken);

console.log('test_observation: OK');
