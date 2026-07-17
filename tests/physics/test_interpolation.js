// Render interpolation of physics transforms (Physics.setInterpolation).
//
// Physics steps at a fixed rate while rendering is uncapped; without
// interpolation anything synced from a body snaps at the fixed-step rate.
// With interpolation ON, render-side consumers (PhysicsNode scene sync,
// getTransform/getAllTransforms with { interpolated: true }) blend the
// previous->current step transforms by the accumulator fraction, so a body
// moving at constant velocity advances smoothly with VIRTUAL time (lagging
// exactly one fixed step). Physics queries (plain getTransform, raycasts)
// always return the true stepped state.
//
// Fixed step 30 Hz; frames advance 10 ms at a time so several rendered
// frames land between steps.

Physics.destroyAll();
Physics.setGravity(0, 0, 0);
Physics.setTimeStep(1 / 30);
const STEP_MS = 1000 / 30;

// --- Default is OFF: interpolated read == true read -----------------------
assert(Physics.getInterpolation() === false, 'interpolation defaults OFF');

const V = 3; // m/s along +x
const b = Physics.createBody({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 0, z: 0 },
    gravityFactor: 0,
    linearDamping: 0,
});
Physics.setLinearVelocity(b, V, 0, 0);

advanceTime(100);
{
    const t = Physics.getTransform(b).position.x;
    const i = Physics.getTransform(b, { interpolated: true }).position.x;
    assert(Math.abs(t - i) < 1e-6,
        'with interpolation OFF, interpolated read equals true read (' + t + ' vs ' + i + ')');
}

// --- ON: interpolated position moves smoothly, strictly between steps -----
Physics.setInterpolation(true);
assert(Physics.getInterpolation() === true, 'interpolation reports ON');
Physics.setLinearVelocity(b, V, 0, 0); // keep it awake and constant

advanceTime(200); // let a few steps run with interpolation active

const samples = [];
for (let f = 0; f < 24; f++) {
    advanceTime(10);
    samples.push({
        t: Physics.getTransform(b).position.x,
        i: Physics.getTransform(b, { interpolated: true }).position.x,
    });
}

// Interpolated value always lies within [true - one step of travel, true].
const stepTravel = V * STEP_MS / 1000; // 0.1 m
for (const s of samples) {
    assert(s.i <= s.t + 1e-4,
        'interpolated never ahead of the true state (' + s.i + ' vs ' + s.t + ')');
    assert(s.i >= s.t - stepTravel - 0.01,
        'interpolated within one step behind the true state (' + s.i + ' vs ' + s.t + ')');
}

// At least one rendered frame strictly between two step positions (a frame
// where the true state did not change but the interpolated one moved).
let strictlyBetween = 0;
for (let k = 1; k < samples.length; k++) {
    const trueHeld = Math.abs(samples[k].t - samples[k - 1].t) < 1e-6;
    const interpMoved = samples[k].i - samples[k - 1].i > 0.005;
    if (trueHeld && interpMoved &&
        samples[k].i > samples[k].t - stepTravel + 0.005 &&
        samples[k].i < samples[k].t - 0.005) strictlyBetween++;
}
assert(strictlyBetween >= 4,
    'interpolated positions land strictly between step positions on held frames, got ' +
    strictlyBetween);

// Smoothness: the interpolated track advances ~V * 10 ms per frame — never a
// full-step snap, never frozen. (True track moves in 0 or ~stepTravel jumps.)
for (let k = 1; k < samples.length; k++) {
    const d = samples[k].i - samples[k - 1].i;
    assert(d > 0.005 && d < 0.08,
        'interpolated per-frame delta ~V*10ms, got ' + d + ' at frame ' + k);
}
// And the true track does snap (proves the two consumers really differ).
let trueJumps = 0;
for (let k = 1; k < samples.length; k++) {
    if (samples[k].t - samples[k - 1].t > stepTravel * 0.7) trueJumps++;
}
assert(trueJumps >= 4, 'true track advances in fixed-step jumps, got ' + trueJumps);

// --- getAllTransforms({interpolated:true}) agrees with getTransform -------
{
    const buf = Physics.getAllTransforms({ interpolated: true });
    let found = false;
    for (let o = 0; o < buf.length; o += 8) {
        if ((buf[o] | 0) === b) {
            const i = Physics.getTransform(b, { interpolated: true }).position.x;
            assert(Math.abs(buf[o + 1] - i) < 1e-4,
                'getAllTransforms interpolated matches getTransform interpolated');
            found = true;
        }
    }
    assert(found, 'body present in getAllTransforms');
}

// --- Teleport snaps (no glide) ---------------------------------------------
Physics.setPosition(b, 100, 0, 0);
{
    const i = Physics.getTransform(b, { interpolated: true }).position;
    assert(Math.abs(i.x - 100) < 1e-4,
        'teleport snaps the interpolated read immediately, got x=' + i.x);
}

// --- Sleeping bodies do not jitter -----------------------------------------
Physics.setGravity(0, -9.81, 0);
Physics.createBody({
    shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
const rest = Physics.createBody({
    shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 0, y: 0.6, z: 0 },
});
advanceTime(3000); // settle + fall asleep
assert(Physics.isActive(rest) === false, 'resting body fell asleep');
for (let f = 0; f < 8; f++) {
    advanceTime(10);
    const t = Physics.getTransform(rest).position;
    const i = Physics.getTransform(rest, { interpolated: true }).position;
    assert(Math.abs(t.x - i.x) + Math.abs(t.y - i.y) + Math.abs(t.z - i.z) < 1e-6,
        'sleeping body renders at its true pose (no jitter)');
}

Physics.setInterpolation(false);
Physics.destroyAll();
