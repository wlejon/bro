// Headless test for bro.portal
assert(typeof bro.portal === 'object', 'bro.portal namespace exists');
if (!bro.portal.available) {
    skipTest('bro.portal is unavailable in this environment');
} else {
    // 1. Service state
    assert(typeof bro.portal.isRunning === 'function', 'bro.portal.isRunning is a function');
    const running = bro.portal.isRunning();
    assert(typeof running === 'boolean', 'isRunning returns boolean');

    // 2. Register file chooser handler
    let fcCalled = false;
    const fcHandle = bro.portal.onFileChooser(async (req) => {
        fcCalled = true;
        return { uris: ['file:///tmp/test.txt'], cancelled: false };
    });
    assert(typeof fcHandle === 'object' && fcHandle !== null, 'onFileChooser returns handle');
    assert(typeof fcHandle.dispose === 'function', 'handle has dispose');

    // 3. Register screenshot handler
    const ssHandle = bro.portal.onScreenshot(async (req) => {
        return { uri: 'file:///tmp/ss.png', cancelled: false };
    });
    assert(typeof ssHandle === 'object' && ssHandle !== null, 'onScreenshot returns handle');

    // 4. Register screencast handler
    const scHandle = bro.portal.onScreencast(async (req) => {
        return { streams: [{ node: 1, source_type: 1 }] };
    });
    assert(typeof scHandle === 'object' && scHandle !== null, 'onScreencast returns handle');

    // 5. Register open URI handler
    const ouHandle = bro.portal.onOpenUri(async (req) => {
        return { cancelled: false };
    });
    assert(typeof ouHandle === 'object' && ouHandle !== null, 'onOpenUri returns handle');

    // 6. Dispose handlers
    fcHandle.dispose();
    ssHandle.dispose();
    scHandle.dispose();
    ouHandle.dispose();

    console.log('test_portal.js PASSED');
}
