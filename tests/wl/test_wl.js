// Headless test for bro.wl:
// In headless mode, bro.wl must not auto-connect to the host Wayland display
// unless a nested test compositor explicitly attaches a display.
assert(typeof bro.wl === 'object', 'bro.wl namespace exists');
assert(typeof bro.wl.available === 'boolean', 'bro.wl.available is boolean');

// 1. Outputs & Seats queries return empty arrays without host connection
const outputs = bro.wl.getOutputs();
assert(Array.isArray(outputs) && outputs.length === 0, 'getOutputs() returns empty array in headless');

const seats = bro.wl.getSeats();
assert(Array.isArray(seats) && seats.length === 0, 'getSeats() returns empty array in headless');

// 2. Protocol capability checks return false without host connection
assert(typeof bro.wl.hasCompositor === 'function' && bro.wl.hasCompositor() === false, 'hasCompositor is false in headless');
assert(typeof bro.wl.hasLayerShell === 'function' && bro.wl.hasLayerShell() === false, 'hasLayerShell is false in headless');
assert(typeof bro.wl.hasForeignToplevelManager === 'function' && bro.wl.hasForeignToplevelManager() === false, 'hasForeignToplevelManager is false in headless');
assert(typeof bro.wl.hasSessionLock === 'function' && bro.wl.hasSessionLock() === false, 'hasSessionLock is false in headless');
assert(typeof bro.wl.hasIdleInhibit === 'function' && bro.wl.hasIdleInhibit() === false, 'hasIdleInhibit is false in headless');
assert(typeof bro.wl.hasScreencopy === 'function' && bro.wl.hasScreencopy() === false, 'hasScreencopy is false in headless');

// 3. Toplevels query returns empty array
const toplevels = bro.wl.getToplevels();
assert(Array.isArray(toplevels) && toplevels.length === 0, 'getToplevels() returns empty array in headless');

// 4. Events subscription works cleanly without connecting
const handle = bro.wl.on('toplevelAdded', () => {});
assert(typeof handle === 'object' && handle !== null, 'on returns handle');
assert(typeof handle.remove === 'function', 'handle has remove method');
handle.remove();

console.log('test_wl.js PASSED');
