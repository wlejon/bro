// Mod matrix: route management, source/dest coverage, external sources.
//
// NOTE: empirically the matrix caps out at ~16 routes — many of the
// source x destination combinations documented in audio-api.js return -1
// rather than a valid route id. Tests below flag that cap as a bug.

const ctx = new AudioContext();
const mm = ctx.getModMatrix();
mm.clearAllRoutes();
assert(mm.routeCount === 0, 'starts at zero routes after clear, got ' + mm.routeCount);

const sources = ['lfo1', 'lfo2', 'lfo3', 'lfo4', 'envelope', 'velocity', 'keytracking', 'modwheel', 'aftertouch'];
const dests   = ['pitch', 'gain', 'pan', 'filterfreq', 'filterq', 'pulsewidth', 'delaysend'];

// Per-source: at minimum, every source should accept at least one destination.
// Per-dest:   at minimum, every destination should accept at least one source.
const succeededBySource = {};
const succeededByDest = {};
const failures = [];
for (const s of sources) {
    for (const d of dests) {
        let id = -1, threw = false;
        try { id = mm.addRoute(s, d, 0.5); } catch (e) { threw = true; }
        if (threw || id < 0) {
            failures.push(s + '->' + d + ' (id=' + id + ')');
        } else {
            succeededBySource[s] = (succeededBySource[s] || 0) + 1;
            succeededByDest[d] = (succeededByDest[d] || 0) + 1;
        }
    }
}
console.log('failures:', failures.length, '/', sources.length * dests.length);
console.log('failures detail:', failures.slice(0, 10).join(', '), failures.length > 10 ? '...' : '');
console.log('succeededBySource:', JSON.stringify(succeededBySource));
console.log('succeededByDest:', JSON.stringify(succeededByDest));
console.log('mm.routeCount:', mm.routeCount);

// All documented (source, dest) combos should be acceptable per audio-api.js.
assert(failures.length === 0,
       'all ' + (sources.length * dests.length) + ' documented source/dest combos accepted, got ' + failures.length + ' failures');

// At minimum every source should be supported at least once.
for (const s of sources) {
    assert((succeededBySource[s] || 0) > 0, 'source "' + s + '" supports at least one destination');
}
// And every destination should be supported.
for (const d of dests) {
    assert((succeededByDest[d] || 0) > 0, 'destination "' + d + '" supports at least one source');
}

// setRouteAmount / setRouteEnabled don't throw on a valid id
mm.clearAllRoutes();
const id = mm.addRoute('lfo1', 'pitch', 0.5);
assert(id >= 0, 'baseline addRoute(lfo1,pitch) returns a valid id, got ' + id);
{
    let threw = false;
    try {
        mm.setRouteAmount(id, 0.25);
        mm.setRouteEnabled(id, false);
        mm.setRouteEnabled(id, true);
    } catch (e) { threw = true; console.log('route mut threw:', e.message); }
    assert(!threw, 'route mutation does not throw');
}

// removeRoute decrements count
const before = mm.routeCount;
mm.removeRoute(id);
assert(mm.routeCount === before - 1, 'routeCount decreased by 1 after removeRoute: ' + before + ' -> ' + mm.routeCount);

// External mod sources
{
    let threw = false;
    try {
        mm.setModWheel(0.7);
        mm.setAftertouch(0.4);
    } catch (e) { threw = true; console.log('extmod threw:', e.message); }
    assert(!threw, 'setModWheel/setAftertouch do not throw');
}

mm.clearAllRoutes();
