// Headless test for bro.sys
assert(typeof bro.sys === 'object', 'bro.sys namespace exists');
assert(bro.sys.available === true, 'bro.sys.available is true for trusted shell app');

// 1. Power
assert(typeof bro.sys.power === 'object', 'bro.sys.power exists');
const powerState = bro.sys.power.getState();
assert(typeof powerState === 'object' && powerState !== null, 'power.getState returns object');
assert(typeof powerState.source === 'string', 'powerState.source is string');
assert(typeof powerState.hasSystemBattery === 'boolean', 'powerState.hasSystemBattery is boolean');
assert(Array.isArray(powerState.devices), 'powerState.devices is array');

// 2. Audio
assert(typeof bro.sys.audio === 'object', 'bro.sys.audio exists');
const audioState = bro.sys.audio.getState();
assert(typeof audioState === 'object' && audioState !== null, 'audio.getState returns object');
assert(Array.isArray(audioState.devices), 'audioState.devices is array');

// 3. Network
assert(typeof bro.sys.network === 'object', 'bro.sys.network exists');
const netState = bro.sys.network.getState();
assert(typeof netState === 'object' && netState !== null, 'network.getState returns object');
assert(typeof netState.connectivity === 'string', 'netState.connectivity is string');
assert(typeof netState.networkingEnabled === 'boolean', 'netState.networkingEnabled is boolean');
assert(Array.isArray(netState.devices), 'netState.devices is array');

const scanPromise = bro.sys.network.scanWifi();
assert(typeof scanPromise === 'object' && typeof scanPromise.then === 'function', 'scanWifi returns Promise');
scanPromise.catch(() => {});

// 4. Bluetooth
assert(typeof bro.sys.bluetooth === 'object', 'bro.sys.bluetooth exists');
const btState = bro.sys.bluetooth.getState();
assert(typeof btState === 'object' && btState !== null, 'bluetooth.getState returns object');
assert(Array.isArray(btState.devices), 'btState.devices is array');

// 5. Notifications
assert(typeof bro.sys.notifications === 'object', 'bro.sys.notifications exists');
assert(typeof bro.sys.notifications.listen === 'function', 'notifications.listen is function');

// 6. Tray
assert(typeof bro.sys.tray === 'object', 'bro.sys.tray exists');
const trayItems = bro.sys.tray.getItems();
assert(Array.isArray(trayItems), 'tray.getItems returns array');

console.log('test_sys.js PASSED');
