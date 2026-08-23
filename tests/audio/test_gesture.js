// Test bro.gesture acoustic non-speech gesture spotter
// Exercises src/js/gesture_bindings.cpp

assert(typeof bro === "object", "bro namespace exists");
assert(typeof bro.gesture === "object", "bro.gesture namespace exists");

// Built without BRO_WITH_SOUNDML (the `app` profile, which is what CI
// builds) leaves bro.gesture as the unavailable stub — a Proxy whose every
// method throws. Assert that contract and stop; the surface below only
// exists in a build that compiled the sibling in.
if (bro.gesture.available === false) {
    let threw = false;
    try {
        bro.gesture.isActive();
    } catch (e) {
        threw = String(e.message).includes("compiled without BRO_WITH_SOUNDML");
    }
    assert(threw, "stub isActive() names the missing build flag");
    console.log("test_gesture: bro.gesture is the unavailable stub; stub contract OK");
} else {
    // 1. Verify API functions
    const methods = [
        "enrollFromAudio", "remove", "clear", "templates", "inspect",
        "reset", "listen", "stop", "isActive", "sampleRate"
    ];
    for (const m of methods) {
        assert(typeof bro.gesture[m] === "function", "bro.gesture." + m + " is function");
    }

    // 2. Initial state
    assert(bro.gesture.isActive() === false, "isActive is false initially");
    assert(bro.gesture.sampleRate() > 0, "sampleRate > 0");
    assert(Array.isArray(bro.gesture.templates()), "templates returns array");
    assert(bro.gesture.templates().length === 0, "templates empty initially");
    assert(bro.gesture.inspect("nonexistent") === null, "inspect nonexistent returns null");

    // 3. Safe idle operations
    bro.gesture.stop();
    bro.gesture.clear();
    bro.gesture.reset();

    // 4. Synthesize and enroll a rhythm gesture from audio
    const rate = bro.gesture.sampleRate();
    const durationSec = 1.0;
    const gesturePcm = new Float32Array(Math.floor(rate * durationSec));

    // Create two sharp onsets at t = 0.2s and t = 0.5s
    const click1 = Math.floor(rate * 0.2);
    const click2 = Math.floor(rate * 0.5);
    for (let i = 0; i < 200; i++) {
        if (click1 + i < gesturePcm.length) gesturePcm[click1 + i] = 0.9 * Math.exp(-i / 30);
        if (click2 + i < gesturePcm.length) gesturePcm[click2 + i] = 0.9 * Math.exp(-i / 30);
    }

    const beats = bro.gesture.enrollFromAudio("double_click", gesturePcm, { minOnsets: 2 });
    assert(typeof beats === "number" && beats >= 1, "enrollFromAudio returns beat count");

    const templates = bro.gesture.templates();
    assert(templates.length === 1 && templates[0] === "double_click", "double_click template is enrolled");

    // Inspect enrolled gesture
    const view = bro.gesture.inspect("double_click");
    assert(typeof view === "object" && view !== null, "inspect returns view object");
    assert(view.name === "double_click", "view.name matches");
    assert(view.kind === "rhythm" || view.kind === "tone", "view.kind is rhythm or tone");
    assert(typeof view.frameMs === "number" && view.frameMs > 0, "view.frameMs > 0");
    assert(Array.isArray(view.intervalsMs), "view.intervalsMs is array");
    assert(Array.isArray(view.onsets), "view.onsets is array");

    // Remove template
    const removed = bro.gesture.remove("double_click");
    assert(removed === true, "remove double_click returns true");
    assert(bro.gesture.templates().length === 0, "templates empty after remove");

    // 5. Parameter validation and error checking
    let enrollThrewNoArg = false;
    try {
        bro.gesture.enrollFromAudio();
    } catch (e) {
        enrollThrewNoArg = true;
    }
    assert(enrollThrewNoArg, "enrollFromAudio without args throws TypeError");

    let removeThrewNoArg = false;
    try {
        bro.gesture.remove();
    } catch (e) {
        removeThrewNoArg = true;
    }
    assert(removeThrewNoArg, "remove without args throws TypeError");

    let listenThrewNoOnGesture = false;
    try {
        bro.gesture.listen({});
    } catch (e) {
        listenThrewNoOnGesture = true;
    }
    assert(listenThrewNoOnGesture, "listen without onGesture throws TypeError");

    console.log("test_gesture: passed");
}
