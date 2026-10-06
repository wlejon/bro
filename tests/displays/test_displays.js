// Headless test for bro.displays
assert(typeof bro.displays === 'object', 'bro.displays namespace exists');
if (!bro.displays.available) {
    skipTest('bro.displays is unavailable in this environment');
} else {
    // 1. Snapshot
    const snap = bro.displays.getSnapshot();
    assert(typeof snap === 'object' && snap !== null, 'getSnapshot returns object');
    assert(Array.isArray(snap.displays), 'snap.displays is array');

    // 2. Night light
    const nl = bro.displays.getNightLight();
    assert(typeof nl === 'object' && nl !== null, 'getNightLight returns object');
    assert(typeof nl.enabled === 'boolean', 'nl.enabled is boolean');
    assert(typeof nl.temperature === 'number', 'nl.temperature is number');

    // 3. Brightness
    const brightness = bro.displays.getBrightness();
    assert(typeof brightness === 'number', 'getBrightness returns number');

    const devices = bro.displays.getBacklightDevices();
    assert(Array.isArray(devices), 'getBacklightDevices returns array');

    console.log('test_displays.js PASSED');
}
