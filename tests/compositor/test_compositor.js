// Headless test for bro.compositor
assert(typeof bro.compositor === 'object', 'bro.compositor namespace exists');
if (!bro.compositor.available) {
    skipTest('bro.compositor is unavailable in this environment');
}

// 1. Windows query
const windows = bro.compositor.getWindows();
assert(Array.isArray(windows), 'getWindows returns array');

// 2. Workspaces query
const workspaces = bro.compositor.getWorkspaces();
assert(Array.isArray(workspaces), 'getWorkspaces returns array');
assert(workspaces.length >= 1, 'at least 1 default workspace exists');
const ws0 = workspaces[0];
assert(typeof ws0.id === 'number', 'workspace has id');
assert(typeof ws0.layoutMode === 'string', 'workspace has layoutMode');

// 3. Layout modes
assert(typeof bro.compositor.setLayoutMode === 'function', 'setLayoutMode is function');
assert(typeof bro.compositor.getLayoutMode === 'function', 'getLayoutMode is function');

// 4. Monitors query
const monitors = bro.compositor.getMonitors();
assert(Array.isArray(monitors), 'getMonitors returns array');

// 5. Events subscription
let receivedEvent = false;
const handle = bro.compositor.on('windowCreated', (ev) => {
    receivedEvent = true;
});
assert(typeof handle === 'object' && handle !== null, 'on returns handle object');
assert(typeof handle.remove === 'function', 'handle has remove method');
handle.remove();

console.log('test_compositor.js PASSED');
