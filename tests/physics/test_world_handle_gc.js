// A world entered and never exited, then collected: the Physics.* calls that
// follow fall back to the default world. The handle's finalizer used to leave
// it on the active-world stack, and the next query read the freed world
// (found by tests/fuzz/test_native_fuzz.js).

(function () {
    const w = bro.physics.createWorldHandle();
    w.enter();
})();
for (let i = 0; i < 300; i++) new Array(100000).fill(i);
runFrames(5);

const hits = bro.physics.overlapPoint(0, 0, 0);
assert(Array.isArray(hits), 'a query after the entered world was collected answers');
console.log('test_world_handle_gc OK');
