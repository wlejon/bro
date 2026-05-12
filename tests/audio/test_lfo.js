// Mod matrix LFOs (4 slots, shapes, rate/depth, sync/free, bipolar).

const ctx = new AudioContext();
const mm = ctx.getModMatrix();
assert(mm, 'getModMatrix returns matrix');

// All 4 LFO indices configurable
const shapes = ['sine', 'triangle', 'square', 'sawup', 'sawdown', 'sampleandhold'];
for (let i = 0; i < 4; i++) {
    for (const s of shapes) {
        let threw = false;
        try { mm.setLfoShape(i, s); } catch (e) { threw = true; console.log('setLfoShape(' + i + ',' + s + ') threw:', e.message); }
        assert(!threw, 'setLfoShape(' + i + ',' + s + ') does not throw'); // BUG: lfo-shape
    }
    let threw = false;
    try {
        mm.setLfoRate(i, 2.0);
        mm.setLfoDepth(i, 0.5);
        mm.setLfoOffset(i, 0.0);
        mm.setLfoBipolar(i, true);
        mm.setLfoSync(i, false);
    } catch (e) { threw = true; console.log('LFO config' + i + ' threw:', e.message); }
    assert(!threw, 'LFO ' + i + ' config does not throw'); // BUG: lfo-config
}

// Bad shape: should throw or silently ignore (not crash)
{
    let threw = false;
    try { mm.setLfoShape(0, 'nonexistent-shape'); } catch (e) { threw = true; }
    console.log('bad shape threw:', threw);
}

// Out-of-range LFO index: should throw or be a no-op (not crash)
{
    let threw = false;
    try { mm.setLfoShape(99, 'sine'); } catch (e) { threw = true; }
    console.log('LFO index 99 threw:', threw);
}

// Functional: LFO modulating pitch should add detectable variance vs static tone.
mm.clearAllRoutes();
mm.setLfoShape(0, 'sine');
mm.setLfoRate(0, 10);
mm.setLfoDepth(0, 1.0);
mm.setLfoBipolar(0, true);
const routeIdx = mm.addRoute('lfo1', 'pitch', 200); // 200 cents of vibrato
console.log('route idx:', routeIdx);
assert(typeof routeIdx === 'number' && routeIdx >= 0, 'addRoute(lfo1,pitch) returns route idx, got ' + routeIdx); // BUG: lfo-route

mm.clearAllRoutes();
assert(mm.routeCount === 0, 'clearAllRoutes resets count, got ' + mm.routeCount); // BUG: clear-routes
