// stop() on a scheduled source that was never started is an InvalidStateError
// DOMException, as in Web Audio (broaudio 609d695). broaudio's own tests run in
// a realm with no DOMException; this checks the DOMException path in bro's.

const ctx = new AudioContext();

function stopError(node, arg) {
    try {
        if (arg === undefined) node.stop(); else node.stop(arg);
    } catch (e) {
        return e;
    }
    return null;
}

for (const [label, make] of [
    ['oscillator', () => ctx.createOscillator()],
    ['buffer source', () => ctx.createBufferSource()],
]) {
    const e = stopError(make());
    assert(e !== null, label + ': stop() before start() throws');
    assert(e.name === 'InvalidStateError', label + ': name is InvalidStateError, got ' + (e && e.name));
    assert(e instanceof DOMException, label + ': the error is a DOMException');
    // The state check comes before the argument check.
    const e2 = stopError(make(), -1);
    assert(e2 && e2.name === 'InvalidStateError', label + ': stop(-1) before start() is still InvalidStateError');
}

// Once started, stop() is fine and a negative time is a RangeError.
const osc = ctx.createOscillator();
osc.connect(ctx.destination);
osc.start();
const neg = stopError(osc, -1);
assert(neg instanceof RangeError, 'started oscillator: stop(-1) is a RangeError');
assert(stopError(osc) === null, 'started oscillator: stop() does not throw');

console.log('test_stop_unstarted: OK');
