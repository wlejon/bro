// Smoke test for the bro.wake JS binding.
//
// Verifies the JS surface is wired correctly without requiring a real mic
// or a labelled audio clip. Loads the trained "computer" model, exercises
// listen / suspend / resume / lastScore / stop and the synchronous feed()
// path with a short burst of silence. A negative score on silence is the
// expected outcome — we only assert that the call returns a number and
// that the suspend/stop bookkeeping flips the binding's state flags.
//
// Run from bro repo root:
//   ./build/Debug/bro-headless.exe ../broworkshop tests/smoke_wake_binding.js

const FS = require('node:fs');

const WEIGHTS = '../brosoundml/weights/wake/computer.bw';

function assert(cond, msg) {
    if (!cond) throw new Error('assert: ' + msg);
}

if (!FS.existsSync(WEIGHTS)) {
    console.log('skip: ' + WEIGHTS + ' not found');
} else {
    let fires = 0;
    bro.wake.listen({
        weights:      WEIGHTS,
        threshold:    0.85,
        refractoryMs: 500,
        onFire:       () => { fires++; },
    });
    assert(bro.wake.isActive(),    'isActive() after listen()');
    assert(!bro.wake.isSuspended(), 'isSuspended() false after listen()');

    // Feed two seconds of zeros — should produce no fire and a low score.
    const sr  = 16000;
    const buf = new Float32Array(sr * 2);
    let fired = false;
    for (let off = 0; off < buf.length; off += 1600) {
        const chunk = buf.subarray(off, Math.min(off + 1600, buf.length));
        fired ||= bro.wake.feed(chunk);
    }
    const score = bro.wake.lastScore();
    assert(typeof score === 'number', 'lastScore returns number');
    console.log('silence score:', score.toFixed(4), 'fired:', fired);

    bro.wake.suspend();
    assert(bro.wake.isSuspended(), 'isSuspended() true after suspend()');
    bro.wake.resume();
    assert(!bro.wake.isSuspended(), 'isSuspended() false after resume()');

    bro.wake.setThreshold(0.55);   // tunable at runtime; just verify no throw.

    bro.wake.stop();
    assert(!bro.wake.isActive(), 'isActive() false after stop()');

    // After stop(), feed() must throw (no detector).
    let threw = false;
    try { bro.wake.feed(new Float32Array(160)); } catch (_) { threw = true; }
    assert(threw, 'feed() throws after stop()');

    // Re-listen so we exercise the implicit-stop path inside listen().
    bro.wake.listen({ weights: WEIGHTS, onFire: () => {} });
    bro.wake.listen({ weights: WEIGHTS, onFire: () => {} });
    assert(bro.wake.isActive(), 'isActive() after re-listen');
    bro.wake.stop();

    console.log('bro.wake smoke OK (fires=' + fires + ')');
}
