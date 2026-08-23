// Test bro.listen stream host, retention buffer, and multi-tenant streams
// Exercises src/js/listen_bindings.cpp and src/js/listen_host.h

assert(typeof bro === "object", "bro namespace exists");
assert(typeof bro.listen === "object", "bro.listen namespace exists");

// Built without BRO_WITH_SOUNDML (the `app` profile, which is what CI
// builds) leaves bro.listen as the unavailable stub — a Proxy whose every
// method throws. Assert that contract and stop; the surface below only
// exists in a build that compiled the sibling in.
if (bro.listen.available === false) {
    let threw = false;
    try {
        bro.listen.supported();
    } catch (e) {
        threw = String(e.message).includes("compiled without BRO_WITH_SOUNDML");
    }
    assert(threw, "stub supported() names the missing build flag");
    console.log("test_listen: bro.listen is the unavailable stub; stub contract OK");
} else {
    // 1. Verify top-level API functions
    const methods = ["open", "supported", "apps", "retain", "audio", "frame", "info"];
    for (const m of methods) {
        assert(typeof bro.listen[m] === "function", "bro.listen." + m + " is function");
    }

    assert(typeof bro.listen.supported() === "boolean", "supported() returns boolean");
    assert(Array.isArray(bro.listen.apps()), "apps() returns array");

    // 2. Retention query on default stream
    bro.listen.retain(4);
    const retInfo = bro.listen.info();
    assert(typeof retInfo === "object" && retInfo !== null, "info() returns object");
    assert(typeof retInfo.active === "boolean", "info.active is boolean");
    assert(typeof retInfo.seconds === "number", "info.seconds is number");
    assert(typeof retInfo.rate === "number", "info.rate is number");
    assert(typeof retInfo.hop === "number", "info.hop is number");
    assert(typeof retInfo.frameRate === "number", "info.frameRate is number");
    assert(typeof retInfo.streamFrame === "number", "info.streamFrame is number");
    assert(typeof retInfo.heldFrames === "number", "info.heldFrames is number");
    assert(typeof retInfo.heldSeconds === "number", "info.heldSeconds is number");

    assert(typeof bro.listen.frame() === "number", "frame() returns number");
    const audioSample = bro.listen.audio(0, 5);
    assert(audioSample === null || audioSample instanceof Float32Array, "audio() returns null or Float32Array");

    bro.listen.retain(0);

    // 3. Open an independent ListenStream
    const stream = bro.listen.open("mic");
    assert(typeof stream === "object" && stream !== null, "open() returns ListenStream");
    assert(typeof stream.id === "number" && stream.id >= 0, "stream.id is valid number");
    assert(stream.kind === "mic", "stream.kind is mic");
    assert(stream.valid === true, "stream.valid is true");

    // Sub-tenant views on stream
    assert(typeof stream.kws === "object" && stream.kws !== null, "stream.kws view exists");
    assert(typeof stream.wake === "object" && stream.wake !== null, "stream.wake view exists");
    assert(typeof stream.sense === "object" && stream.sense !== null, "stream.sense view exists");
    assert(typeof stream.gesture === "object" && stream.gesture !== null, "stream.gesture view exists");

    // Stream retention and feed
    stream.retain(2);
    const sInfo = stream.info();
    assert(typeof sInfo === "object" && sInfo !== null, "stream.info() returns object");
    assert(typeof sInfo.seconds === "number", "stream.info.seconds is number");
    assert(typeof stream.frame() === "number", "stream.frame() returns number");

    const feedSize = sInfo.rate > 0 ? Math.floor(sInfo.rate / 4) : 4000;
    stream.feed(new Float32Array(feedSize));

    // Close stream
    stream.close();
    assert(stream.valid === false, "stream.valid is false after close()");

    console.log("test_listen: passed");
}
