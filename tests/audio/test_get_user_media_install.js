// navigator.mediaDevices.getUserMedia reaches broaudio's capture whichever
// installs first: the navigator (with its rejecting stub) or broaudio's
// installAudio (which defines the global __nativeGetUserMedia and, when the
// navigator already exists, puts it on navigator.mediaDevices).
//
// The real call opens the default capture device, so this test never makes
// it; it swaps the native global for a stand-in and checks the route.

if (typeof AudioContext === 'undefined') {
    console.log('skip: audio not compiled in');
} else {
    assert(typeof navigator.mediaDevices === 'object' && navigator.mediaDevices !== null,
           'navigator.mediaDevices exists');
    const gum = navigator.mediaDevices.getUserMedia;
    assert(typeof gum === 'function', 'getUserMedia is a function');
    assert(typeof __nativeGetUserMedia === 'function', 'broaudio installed __nativeGetUserMedia');

    if (gum === __nativeGetUserMedia) {
        // installAudio ran after the navigator and replaced the stub.
        console.log('getUserMedia is the native function');
    } else {
        // The navigator's function must forward to the native one at call
        // time rather than reject with its NotSupportedError.
        const real = __nativeGetUserMedia;
        const marker = Promise.resolve('routed');
        let seen;
        globalThis.__nativeGetUserMedia = function (c) { seen = c; return marker; };
        const constraints = { audio: true };
        const got = navigator.mediaDevices.getUserMedia(constraints);
        globalThis.__nativeGetUserMedia = real;
        assert(got === marker, 'getUserMedia returns what the native function returns');
        assert(seen === constraints, 'getUserMedia passes its constraints through');

        // A native function that throws turns into a rejected promise.
        globalThis.__nativeGetUserMedia = function () { throw new Error('boom'); };
        const p = navigator.mediaDevices.getUserMedia({ audio: true });
        globalThis.__nativeGetUserMedia = real;
        assert(p instanceof Promise, 'a throwing native call still returns a promise');
        let reason;
        await p.then(() => {}, (e) => { reason = e; });
        assert(reason && reason.message === 'boom', 'the throw becomes the rejection reason');
    }
}
