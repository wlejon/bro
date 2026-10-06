// Headless test for bro.wl
assert(typeof bro.wl === 'object', 'bro.wl namespace exists');
if (!bro.wl.available) {
    skipTest('bro.wl is unavailable in this environment');
}

// 1. Outputs & Seats queries
const outputs = bro.wl.getOutputs();
assert(Array.isArray(outputs), 'getOutputs returns array');

const seats = bro.wl.getSeats();
assert(Array.isArray(seats), 'getSeats returns array');

// 2. Protocol capability checks
assert(typeof bro.wl.hasCompositor === 'function', 'hasCompositor is function');
assert(typeof bro.wl.hasLayerShell === 'function', 'hasLayerShell is function');
assert(typeof bro.wl.hasForeignToplevelManager === 'function', 'hasForeignToplevelManager is function');
assert(typeof bro.wl.hasSessionLock === 'function', 'hasSessionLock is function');
assert(typeof bro.wl.hasIdleInhibit === 'function', 'hasIdleInhibit is function');
assert(typeof bro.wl.hasScreencopy === 'function', 'hasScreencopy is function');

// 3. Toplevels query
const toplevels = bro.wl.getToplevels();
assert(Array.isArray(toplevels), 'getToplevels returns array');

// 4. Events subscription
let receivedToplevel = false;
const handle = bro.wl.on('toplevelAdded', (ev) => {
    receivedToplevel = true;
});
assert(typeof handle === 'object' && handle !== null, 'on returns handle');
assert(typeof handle.remove === 'function', 'handle has remove method');
handle.remove();

console.log('test_wl.js PASSED');
