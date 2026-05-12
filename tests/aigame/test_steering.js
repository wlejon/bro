// Steering primitives: seek, arrive, flee, pursue, evade.

const G = bro.ai.game;
const S = G.steer;

function isFiniteNum(v) { return typeof v === 'number' && Number.isFinite(v); }
function isVec(v, name) {
    assert(v && typeof v === 'object', name + ' is object');
    assert(isFiniteNum(v.fx) && isFiniteNum(v.fz), name + ' has finite fx/fz, got ' + JSON.stringify(v));
}

// --- shape checks ---
isVec(S.seek(0, 0, 10, 0), 'seek');
isVec(S.arrive(0, 0, 10, 0, 1.5), 'arrive');
isVec(S.flee(0, 0, 10, 0), 'flee');
isVec(S.pursue(0, 0, 10, 0, 0, 0, 6), 'pursue');
isVec(S.evade(0, 0, 10, 0, 0, 0, 6), 'evade');

// seek: direction should point toward target
{
    const v = S.seek(0, 0, 10, 0);
    assert(v.fx > 0 && Math.abs(v.fz) < 0.01, 'seek points +x toward (10,0): ' + JSON.stringify(v));
}
// flee: direction should point away from threat
{
    const v = S.flee(0, 0, 10, 0);
    assert(v.fx < 0 && Math.abs(v.fz) < 0.01, 'flee points -x from (10,0): ' + JSON.stringify(v));
}

// arrive: magnitude smaller close to target than far from target.
{
    const far  = S.arrive(0, 0, 10, 0, 1.5);
    const near = S.arrive(9.5, 0, 10, 0, 1.5);
    const mFar = Math.hypot(far.fx, far.fz);
    const mNear = Math.hypot(near.fx, near.fz);
    assert(mNear < mFar, 'arrive slows near target: near=' + mNear.toFixed(3) + ' far=' + mFar.toFixed(3));
}

// pursue: with target moving +x, predicted intercept should bias east of target
{
    const dir = S.pursue(0, 0, 10, 0,  5, 0, 6);
    // target moving away in +x => intercept beyond, so desired vel still +x.
    assert(dir.fx > 0, 'pursue toward fleeing target +x: ' + JSON.stringify(dir));
}

// evade: with threat moving toward self, flee predicted future position
{
    const dir = S.evade(0, 0, 10, 0,  -5, 0, 6);  // threat moving toward us
    assert(dir.fx < 0, 'evade from approaching threat is -x: ' + JSON.stringify(dir));
}

// --- Agent-driven convergence: seek via Agent.update ---
{
    const nav = G.createNavGrid({ minX: -30, minZ: -30, maxX: 30, maxZ: 30, cellSize: 0.5 });
    const bot = G.createAgent({ navGrid: nav, x: -10, z: 0, speed: 6, radius: 0.4 });
    bot.setTarget(10, 0);
    const dt = 1/60;
    let arrived = false;
    for (let i = 0; i < 600; i++) {
        bot.update(dt);
        if (bot.atTarget) { arrived = true; break; }
    }
    assert(arrived, 'agent reached target within 10s, x=' + bot.x.toFixed(2) + ' z=' + bot.z.toFixed(2));
    assert(Math.hypot(bot.x - 10, bot.z - 0) < 1.5, 'agent close to target: (' + bot.x.toFixed(2) + ',' + bot.z.toFixed(2) + ')');
}

// --- Agent does NOT move with no target ---
{
    const bot2 = G.createAgent({ x: 0, z: 0, speed: 6 });
    for (let i = 0; i < 60; i++) bot2.update(1/60);
    assert(Math.abs(bot2.x) < 0.01 && Math.abs(bot2.z) < 0.01,
        'agent without target should not move, got (' + bot2.x + ',' + bot2.z + ')');
}

console.log('test_steering: OK');
