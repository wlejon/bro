// Test bro.kws bindings and API surface
// Exercises src/js/kws_bindings.cpp

assert(typeof bro === "object", "bro namespace exists");
assert(typeof bro.kws === "object", "bro.kws namespace exists");

// 1. Verify API functions
const methods = [
    "load", "unload", "enroll", "enrollFromAudio", "enrollFromClasses",
    "inspect", "remove", "clear", "templates", "reset",
    "listen", "stop", "suspend", "resume",
    "isActive", "isSuspended", "isLoaded", "sampleRate",
    "prefixProgress", "progress", "posterior", "stats", "feed"
];
for (const m of methods) {
    assert(typeof bro.kws[m] === "function", "bro.kws." + m + " is function");
}

// 2. Initial state queries
assert(bro.kws.isLoaded() === false, "isLoaded is false initially");
assert(bro.kws.isActive() === false, "isActive is false initially");
assert(bro.kws.isSuspended() === false, "isSuspended is false initially");
assert(bro.kws.sampleRate() === 0, "sampleRate is 0 when no net loaded");
assert(bro.kws.prefixProgress() === 0.0, "prefixProgress is 0.0");
assert(bro.kws.progress() === null, "progress is null");
assert(bro.kws.posterior() === null, "posterior is null");
assert(bro.kws.inspect("nonexistent") === null, "inspect returns null for missing template");
assert(bro.kws.stats() === null, "stats is null when inactive");

const initialTemplates = bro.kws.templates();
assert(Array.isArray(initialTemplates), "templates returns array");
assert(initialTemplates.length === 0, "templates is empty initially");

// 3. Safe idle operations
bro.kws.suspend();
bro.kws.resume();
bro.kws.stop();
bro.kws.clear();
bro.kws.reset();
bro.kws.unload();

// 4. Parameter validation and error checking
let loadThrewNoArg = false;
try {
    bro.kws.load();
} catch (e) {
    loadThrewNoArg = true;
}
assert(loadThrewNoArg, "load without arguments throws TypeError");

let loadThrewNoWeights = false;
try {
    bro.kws.load({});
} catch (e) {
    loadThrewNoWeights = true;
}
assert(loadThrewNoWeights, "load without weights throws TypeError");

let enrollThrewBeforeLoad = false;
try {
    bro.kws.enroll("test", [1, 2, 3]);
} catch (e) {
    enrollThrewBeforeLoad = true;
}
assert(enrollThrewBeforeLoad, "enroll before load throws Error");

let enrollAudioThrewBeforeLoad = false;
try {
    bro.kws.enrollFromAudio("test", new Float32Array(1600));
} catch (e) {
    enrollAudioThrewBeforeLoad = true;
}
assert(enrollAudioThrewBeforeLoad, "enrollFromAudio before load throws Error");

let enrollClassesThrewBeforeLoad = false;
try {
    bro.kws.enrollFromClasses("test", [1, 2]);
} catch (e) {
    enrollClassesThrewBeforeLoad = true;
}
assert(enrollClassesThrewBeforeLoad, "enrollFromClasses before load throws Error");

let listenThrewBeforeLoad = false;
try {
    bro.kws.listen({ onSpot: () => {} });
} catch (e) {
    listenThrewBeforeLoad = true;
}
assert(listenThrewBeforeLoad, "listen before load throws Error");

let feedThrewInactive = false;
try {
    bro.kws.feed(new Float32Array(1600));
} catch (e) {
    feedThrewInactive = true;
}
assert(feedThrewInactive, "feed on inactive stream throws Error");

console.log("test_kws: passed");
