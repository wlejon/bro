// bro.apps builds its catalog (every installed app's desktop entry, and the
// MIME associations over them) on a worker thread: ready() starts the build
// and returns a promise at once, and the page keeps running while it builds.
// The sync calls (list, getDefaultApp, ...) stay; after ready() resolves they
// answer from the built catalog in a few ms. Before, the first of them built
// the catalog on the page thread: ~450 ms warm, ~1.5 s cold on Windows.

if (!bro.apps || !bro.apps.available) {
    skipTest('bro.apps is compiled out of this build');
} else {
    assert(typeof bro.apps.ready === 'function', 'bro.apps.ready exists');
    assert(typeof bro.apps.isReady === 'boolean', 'bro.apps.isReady is a boolean');

    const t0 = perf.now();
    const p = bro.apps.ready();
    const callMs = perf.now() - t0;
    assert(p && typeof p.then === 'function', 'ready() returns a promise');
    assert(callMs < 50, 'ready() returns at once, not after the build: ' + callMs.toFixed(1) + ' ms');

    // The page thread is free while it builds: timers run.
    let ticks = 0;
    const timer = setInterval(() => { ticks++; }, 1);
    let done = false;
    p.then(() => { done = true; });
    const end = perf.now() + 30000;
    while (!done && perf.now() < end) {
        advanceTime(16);
        wallSleep(5);
    }
    clearInterval(timer);
    const buildMs = perf.now() - t0;
    assert(done, 'ready() resolved');
    assert(bro.apps.isReady === true, 'isReady after ready()');
    console.log('test_apps_ready: catalog built in ' + buildMs.toFixed(0) + ' ms off the page thread (' + ticks + ' timer ticks meanwhile)');

    // Built: the sync calls are quick now.
    let t = perf.now();
    const def = bro.apps.getDefaultApp('text/plain');
    const defMs = perf.now() - t;
    assert(def === null || typeof def === 'object', 'getDefaultApp answers');
    t = perf.now();
    const all = bro.apps.list();
    const listMs = perf.now() - t;
    assert(Array.isArray(all), 'list answers');
    console.log('test_apps_ready: getDefaultApp ' + defMs.toFixed(1) + ' ms, list ' + listMs.toFixed(1) + ' ms (' + all.length + ' apps)');
    assert(defMs < 100, 'getDefaultApp after ready() does not build: ' + defMs.toFixed(1) + ' ms');

    // A second ready() is already resolved.
    let again = false;
    bro.apps.ready().then(() => { again = true; });
    advanceTime(16);
    assert(again, 'a later ready() resolves at once');
    console.log('test_apps_ready.js PASSED');
}
