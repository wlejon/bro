// Test requestAnimationFrame with advanceTime

let rafCalled = false;
let rafTimestamp = 0;

requestAnimationFrame((ts) => {
    rafCalled = true;
    rafTimestamp = ts;
});

// Should not fire until time advances
flush();
assert(!rafCalled, 'rAF not called immediately');

// Advance one frame (~16ms)
advanceTime(16);
assert(rafCalled, 'rAF called after advanceTime(16)');
assert(typeof rafTimestamp === 'number', 'rAF receives timestamp');

// rAF is one-shot - should not fire again
rafCalled = false;
advanceTime(16);
assert(!rafCalled, 'rAF does not repeat');

// cancelAnimationFrame
let cancelled = false;
const id = requestAnimationFrame(() => { cancelled = true; });
cancelAnimationFrame(id);
advanceTime(16);
assert(!cancelled, 'cancelAnimationFrame prevents callback');
