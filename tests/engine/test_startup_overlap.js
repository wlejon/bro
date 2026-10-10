// Headless startup brings the GPU up (the Vulkan loader and driver, instance
// and device, the presenter, Skia's context: ~160 ms on Windows) on a thread
// while the main thread creates the hidden window (SDL's video init and the
// window: ~90 ms), so the graphics cost about the longer of the two instead
// of their sum. bro.app.startup.graphics reports the three durations of this
// launch. (One after the other they were ~175 ms; together ~163 ms, and the
// first frame of an empty page went from ~240 ms to ~220 ms. The window's
// share was smaller than its 90 ms: SDL was loading the Vulkan driver the
// device then reused.)

const s = bro.app.startup;
assert(s && typeof s.graphics === 'object', 'bro.app.startup.graphics: ' + JSON.stringify(s));
const { windowMs, gpuMs, totalMs } = s.graphics;
console.log('test_startup_overlap: window ' + windowMs.toFixed(1) + ' ms, gpu ' + gpuMs.toFixed(1) +
    ' ms, both ' + totalMs.toFixed(1) + ' ms; page loaded at ' + s.loadedMs.toFixed(0) + ' ms');
assert(windowMs > 0 && totalMs > 0, 'the phases were timed');
if (gpuMs < 0) {
    console.log('test_startup_overlap: no GPU in this run, nothing to overlap');
} else {
    const serial = windowMs + gpuMs;
    const saved = serial - totalMs;
    const shorter = Math.min(windowMs, gpuMs);
    assert(totalMs >= Math.max(windowMs, gpuMs) - 1, 'both finished inside the total');
    // One after the other, saved would be ~0. Running together saves about
    // the shorter phase; half of it leaves room for a loaded machine.
    assert(saved > 0.5 * shorter,
        'the window and the GPU came up together: ' + totalMs.toFixed(1) + ' ms for ' + windowMs.toFixed(1) +
        ' + ' + gpuMs.toFixed(1) + ' ms (saved ' + saved.toFixed(1) + ' of ' + shorter.toFixed(1) + ' possible)');
}
console.log('test_startup_overlap.js PASSED');
