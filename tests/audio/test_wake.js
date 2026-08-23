// Test bro.wake bindings and API surface
// Exercises src/js/wake_bindings.cpp

assert(typeof bro === "object", "bro namespace exists");
assert(typeof bro.wake === "object", "bro.wake namespace exists");

// Built without BRO_WITH_SOUNDML (the `app` profile, which is what CI
// builds) leaves bro.wake as the unavailable stub — a Proxy whose every
// method throws. Assert that contract and stop; the surface below only
// exists in a build that compiled the sibling in.
if (bro.wake.available === false) {
    let threw = false;
    try {
        bro.wake.isLoaded();
    } catch (e) {
        threw = String(e.message).includes("compiled without BRO_WITH_SOUNDML");
    }
    assert(threw, "stub isLoaded() names the missing build flag");
    console.log("test_wake: bro.wake is the unavailable stub; stub contract OK");
} else {
    // 1. Check all API functions exist
    assert(typeof bro.wake.load === "function", "bro.wake.load is function");
    assert(typeof bro.wake.unload === "function", "bro.wake.unload is function");
    assert(typeof bro.wake.listen === "function", "bro.wake.listen is function");
    assert(typeof bro.wake.stop === "function", "bro.wake.stop is function");
    assert(typeof bro.wake.suspend === "function", "bro.wake.suspend is function");
    assert(typeof bro.wake.resume === "function", "bro.wake.resume is function");
    assert(typeof bro.wake.lastScore === "function", "bro.wake.lastScore is function");
    assert(typeof bro.wake.isActive === "function", "bro.wake.isActive is function");
    assert(typeof bro.wake.isSuspended === "function", "bro.wake.isSuspended is function");
    assert(typeof bro.wake.isLoaded === "function", "bro.wake.isLoaded is function");
    assert(typeof bro.wake.setThreshold === "function", "bro.wake.setThreshold is function");
    assert(typeof bro.wake.stats === "function", "bro.wake.stats is function");
    assert(typeof bro.wake.feed === "function", "bro.wake.feed is function");

    // 2. Initial state queries
    assert(bro.wake.isLoaded() === false, "isLoaded is false initially");
    assert(bro.wake.isActive() === false, "isActive is false initially");
    assert(bro.wake.isSuspended() === false, "isSuspended is false initially");
    assert(bro.wake.lastScore() === 0.0, "lastScore is 0.0 initially");
    assert(bro.wake.stats() === null, "stats is null when inactive");

    // 3. Threshold adjustment
    bro.wake.setThreshold(0.75);

    // 4. Safe control ops when idle
    bro.wake.suspend();
    bro.wake.resume();
    bro.wake.stop();
    bro.wake.unload();

    // 5. Parameter validation and error checking
    let loadThrewNoArg = false;
    try {
        bro.wake.load();
    } catch (e) {
        loadThrewNoArg = true;
    }
    assert(loadThrewNoArg, "load without arguments throws TypeError");

    let loadThrewNoWeights = false;
    try {
        bro.wake.load({});
    } catch (e) {
        loadThrewNoWeights = true;
    }
    assert(loadThrewNoWeights, "load without weights throws TypeError");

    let listenThrewNoOnFire = false;
    try {
        bro.wake.listen({});
    } catch (e) {
        listenThrewNoOnFire = true;
    }
    assert(listenThrewNoOnFire, "listen without onFire throws TypeError");

    let feedThrewInactive = false;
    try {
        bro.wake.feed(new Float32Array(1600));
    } catch (e) {
        feedThrewInactive = true;
    }
    assert(feedThrewInactive, "feed on inactive stream throws Error");

    console.log("test_wake: passed");
}
