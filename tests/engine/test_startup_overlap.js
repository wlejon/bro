// Startup overlaps the GPU with everything that does not need it.
//
// The GPU (the Vulkan loader and driver, instance and device, the presenter,
// Skia's context: ~160-190 ms on Windows) is the long pole. Headless brings
// it up on a thread while the main thread creates the hidden window (SDL's
// video init: ~90 ms), then installs the realm's host globals (~45 ms) and
// starts compiling the page's first script on a thread of its own; the
// script runs once the device is up (Engine::beginPageCompile). Windowed
// bro does the same with the device on the main thread. bro.app.startup
// reports it: graphics { windowMs, gpuMs, totalMs, startAtMs, readyAtMs } and
// page { globalsAtMs, globalsMs, compileStartMs, compileEndMs, compileMs,
// waitMs, codeCache, overlapMs }. (Window and GPU one after the other were
// ~175 ms, together ~163 ms; with the page's host globals and compile also
// under the device, music's first frame went from ~323 to ~236 ms, warm
// code cache.)
// BRO_STARTUP_OVERLAP=0 compiles after the GPU instead, as bro did.

const s = bro.app.startup;
assert(s && typeof s.graphics === 'object', 'bro.app.startup.graphics: ' + JSON.stringify(s));
const { windowMs, gpuMs, totalMs, readyAtMs } = s.graphics;
console.log('test_startup_overlap: window ' + windowMs.toFixed(1) + ' ms, gpu ' + gpuMs.toFixed(1) +
    ' ms, both ' + totalMs.toFixed(1) + ' ms; page loaded at ' + s.loadedMs.toFixed(0) + ' ms');
assert(windowMs > 0 && totalMs > 0, 'the phases were timed');
if (gpuMs < 0) {
    console.log('test_startup_overlap: no GPU in this run, nothing to overlap');
} else {
    const serial = windowMs + gpuMs;
    const saved = serial - totalMs;
    const shorter = Math.min(windowMs, gpuMs);
    // One after the other, saved would be ~0. Running together saves about
    // the shorter phase; half of it leaves room for a loaded machine.
    assert(saved > 0.5 * shorter,
        'the window and the GPU came up together: ' + totalMs.toFixed(1) + ' ms for ' + windowMs.toFixed(1) +
        ' + ' + gpuMs.toFixed(1) + ' ms (saved ' + saved.toFixed(1) + ' of ' + shorter.toFixed(1) + ' possible)');

    // The page (tests/test_app has a classic script) was compiled while the
    // device came up, not after it.
    const p = s.page;
    assert(p && typeof p === 'object', 'bro.app.startup.page: ' + JSON.stringify(s));
    console.log('test_startup_overlap: globals at ' + p.globalsAtMs.toFixed(1) + ' (' + p.globalsMs.toFixed(1) +
        ' ms), compile ' + p.compileStartMs.toFixed(1) + '-' + p.compileEndMs.toFixed(1) + ' (code cache ' +
        p.codeCache + '), GPU ready at ' + readyAtMs.toFixed(1) + ', waited ' + p.waitMs.toFixed(1) +
        ' ms, ' + p.overlapMs.toFixed(1) + ' ms under the GPU');
    assert(readyAtMs > 0, 'the GPU was ready at a time: ' + readyAtMs);
    assert(p.globalsAtMs >= 0 && p.globalsAtMs < readyAtMs,
        'the host globals were installed before the GPU was up: ' + p.globalsAtMs + ' vs ' + readyAtMs);
    assert(p.compileStartMs >= 0 && p.compileStartMs < readyAtMs,
        'the page began compiling before the GPU was up: ' + p.compileStartMs + ' vs ' + readyAtMs);
    assert(p.compileEndMs >= p.compileStartMs && p.compileEndMs <= s.loadedMs,
        'and finished before the page ran: ' + p.compileEndMs + ' vs loaded ' + s.loadedMs);
    assert(p.overlapMs > 0, 'some of it ran under the GPU: ' + p.overlapMs);
    assert(p.codeCache === 'hit' || p.codeCache === 'miss' || p.codeCache === 'off', 'code cache: ' + p.codeCache);
}
console.log('test_startup_overlap.js PASSED');
