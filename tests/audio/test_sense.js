// Test bro.sense bindings and acoustic sensing hub
// Exercises src/js/sense_bindings.cpp

assert(typeof bro === "object", "bro namespace exists");
assert(typeof bro.sense === "object", "bro.sense namespace exists");

// 1. Check API methods
const methods = ["start", "stop", "isActive", "snapshot", "sampleRate", "stats", "feed", "analyze"];
for (const m of methods) {
    assert(typeof bro.sense[m] === "function", "bro.sense." + m + " is function");
}

// 2. Initial state
assert(bro.sense.isActive() === false, "isActive is false initially");
assert(bro.sense.snapshot() === null, "snapshot is null when inactive");
assert(bro.sense.sampleRate() === 0, "sampleRate is 0 when inactive");
assert(bro.sense.stats() === null, "stats is null when inactive");

// 3. Offline analyze() test
const sampleRate = 16000;
const testPcm = new Float32Array(sampleRate);
for (let i = 0; i < testPcm.length; i++) {
    // 440 Hz tone with ramp
    testPcm[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sampleRate);
}

const analysis = bro.sense.analyze(testPcm);
assert(typeof analysis === "object" && analysis !== null, "analyze returns object");
assert(typeof analysis.frames === "number" && analysis.frames > 0, "analysis.frames > 0");
assert(typeof analysis.hop === "number" && analysis.hop > 0, "analysis.hop > 0");
assert(typeof analysis.win === "number" && analysis.win > 0, "analysis.win > 0");
assert(typeof analysis.rate === "number" && analysis.rate > 0, "analysis.rate > 0");
assert(typeof analysis.frameMs === "number" && analysis.frameMs > 0, "analysis.frameMs > 0");
assert(analysis.db instanceof Float32Array, "analysis.db is Float32Array");
assert(analysis.dominantHz instanceof Float32Array, "analysis.dominantHz is Float32Array");
assert(analysis.periodicity instanceof Float32Array, "analysis.periodicity is Float32Array");
assert(analysis.centroid instanceof Float32Array, "analysis.centroid is Float32Array");
assert(analysis.flags instanceof Int32Array, "analysis.flags is Int32Array");
assert(analysis.db.length === analysis.frames, "db length matches frames");

// 4. Live start, feed, snapshot, stop cycle
bro.sense.start({
    vadFloorDb: -55,
    vadSnrDb: 6,
    onsetRatio: 1.5,
    tonalMinPeriodicity: 0.6,
});

assert(bro.sense.isActive() === true, "isActive is true after start()");
const hubRate = bro.sense.sampleRate();
assert(hubRate > 0, "hub sampleRate > 0");

// Feed audio chunk in headless mode
const feedPcm = new Float32Array(Math.floor(hubRate / 4));
for (let i = 0; i < feedPcm.length; i++) {
    feedPcm[i] = 0.3 * Math.sin(2 * Math.PI * 440 * i / hubRate);
}

const snap1 = bro.sense.feed(feedPcm);
assert(typeof snap1 === "object" && snap1 !== null, "feed returns snapshot in headless");
assert(typeof snap1.frames === "number", "snapshot has frames");
assert(typeof snap1.t === "number", "snapshot has t");
assert(typeof snap1.rms === "number", "snapshot has rms");
assert(typeof snap1.peak === "number", "snapshot has peak");
assert(typeof snap1.db === "number", "snapshot has db");
assert(typeof snap1.voice === "boolean", "snapshot has voice boolean");
assert(typeof snap1.onset === "boolean", "snapshot has onset boolean");
assert(typeof snap1.tonal === "boolean", "snapshot has tonal boolean");
assert(typeof snap1.dominantHz === "number", "snapshot has dominantHz");

const snap2 = bro.sense.snapshot();
assert(typeof snap2 === "object" && snap2 !== null, "snapshot() returns valid object while active");

bro.sense.stop();
assert(bro.sense.isActive() === false, "isActive is false after stop()");

console.log("test_sense: passed");
