// Vectorized batched 1v1 simulator.

const G = bro.ai.game;

const N = 4;
const vec = G.createVecSimulation({
    numEnvs: N, arenaHalfSize: 10, dt: 1/60, maxStepsPerEpisode: 120,
    hp: 100, damage: 5, attackRange: 2.5, moveSpeed: 6,
    rewardDamageDealt: 1.0, rewardKill: 10, rewardDeath: -10,
});

assert(vec.numEnvs === N, 'numEnvs=N, got ' + vec.numEnvs);

vec.seedAndReset(0xC0DEn);

// observe(team) -> Float32Array length N * OBS_TOTAL.
const obs1 = vec.observe(1);
assert(obs1 instanceof Float32Array, 'observe returns Float32Array');
assert(obs1.length === N * G.OBS_TOTAL,
    'observe len = N * OBS_TOTAL, got ' + obs1.length + ' vs ' + (N * G.OBS_TOTAL));

// actionMask(team) -> {mask, enemyIds}.
const am = vec.actionMask(1);
assert(am && am.mask && am.enemyIds, 'actionMask returns {mask, enemyIds}');
assert(am.mask instanceof Float32Array, 'mask is Float32Array');

// applyActions: array of N AgentAction objects.
const acts = [];
for (let i = 0; i < N; i++) {
    acts.push({ moveX: 1, moveZ: 0, aimYaw: 0, aimPitch: 0,
                attackTargetId: -1, abilitySlot: -1, abilityTargetId: -1 });
}
vec.applyActions(1, acts);
vec.applyActions(2, acts);

vec.step();

const sc = vec.stepCounts();
// stepCounts may be Int32Array or array.
const arr = (sc instanceof Int32Array || Array.isArray(sc)) ? sc : null;
assert(arr, 'stepCounts is array-like, got ' + typeof sc);

let totalSteps = 0;
for (let i = 0; i < N; i++) totalSteps += arr[i];
assert(totalSteps >= N, 'each env stepped at least once, total=' + totalSteps);

const dones = vec.dones();
assert(dones && 'done' in dones && 'winner' in dones, 'dones has {done, winner}');
assert(dones.done.length === N, 'dones.done len=N');

const rewards = vec.rewards();
assert(rewards && 'hero' in rewards && 'opponent' in rewards, 'rewards has hero/opponent');
assert(rewards.hero.length === N, 'rewards.hero len=N');

vec.resetEnv(0);
vec.resetDone();

console.log('test_vecsim: OK');
