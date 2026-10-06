// bro.window — system tray, global hotkeys, single instance.

assert(typeof bro === 'object', 'bro namespace exists');
assert(typeof bro.window === 'object', 'bro.window exists');

// ---- System Tray -------------------------------------------------------------
assert(typeof bro.window.isTrayAvailable === 'function', 'isTrayAvailable exists');
assert(typeof bro.window.hasTray === 'function', 'hasTray exists');
assert(typeof bro.window.setTray === 'function', 'setTray exists');
assert(typeof bro.window.removeTray === 'function', 'removeTray exists');

assert(bro.window.isTrayAvailable() === true, 'tray is available');
assert(bro.window.hasTray() === false, 'starts without tray');

let item1Clicked = false;
let item2Clicked = false;

const setOk = bro.window.setTray({
    icon: 'app://icon.png',
    tooltip: 'My Tray App',
    menu: [
        { id: 'item1', label: 'Item 1', click: () => { item1Clicked = true; } },
        { id: 'item2', label: 'Item 2', click: () => { item2Clicked = true; } },
    ],
});
assert(setOk === true, 'setTray returns true');
assert(bro.window.hasTray() === true, 'hasTray is true after setTray');

// Simulate menu clicks
__bro_native.window.simulateTrayClick('item1');
assert(item1Clicked === true, 'item 1 click handler fired');
assert(item2Clicked === false, 'item 2 click handler not fired yet');

__bro_native.window.simulateTrayClick('item2');
assert(item2Clicked === true, 'item 2 click handler fired');

assert(bro.window.removeTray() === true, 'removeTray returns true');
assert(bro.window.hasTray() === false, 'hasTray is false after removeTray');

// ---- Global Hotkeys ----------------------------------------------------------
assert(typeof bro.window.registerGlobalHotkey === 'function', 'registerGlobalHotkey exists');
assert(typeof bro.window.unregisterGlobalHotkey === 'function', 'unregisterGlobalHotkey exists');
assert(typeof bro.window.unregisterAllGlobalHotkeys === 'function', 'unregisterAllGlobalHotkeys exists');

let hotkeyFired = 0;
let lastHotkeyAccel = '';

const hkId = bro.window.registerGlobalHotkey('CommandOrControl+Shift+K', (e) => {
    hotkeyFired++;
    lastHotkeyAccel = e.accelerator;
});
assert(typeof hkId === 'number' && hkId > 0, 'hotkey registered with id > 0: ' + hkId);

// Simulate hotkey trigger
const simOk = __bro_native.window.simulateGlobalHotkey('CommandOrControl+Shift+K');
assert(simOk === true, 'simulateGlobalHotkey returns true');
assert(hotkeyFired === 1, 'hotkey callback fired once');

// Unregister single hotkey
const unregOk = bro.window.unregisterGlobalHotkey(hkId);
assert(unregOk === true, 'unregisterGlobalHotkey returns true');

// After unregister, simulation returns false (not registered)
const simAfter = __bro_native.window.simulateGlobalHotkey('CommandOrControl+Shift+K');
assert(simAfter === false, 'simulateGlobalHotkey returns false after unregister');
assert(hotkeyFired === 1, 'hotkey callback not fired again');

// Register multiple hotkeys and unregisterAll
const hk1 = bro.window.registerGlobalHotkey('Alt+F1', () => {});
const hk2 = bro.window.registerGlobalHotkey('Alt+F2', () => {});
assert(hk1 > 0 && hk2 > 0, 'multiple hotkeys registered');
bro.window.unregisterAllGlobalHotkeys();
assert(__bro_native.window.simulateGlobalHotkey('Alt+F1') === false, 'hk1 unregistered');
assert(__bro_native.window.simulateGlobalHotkey('Alt+F2') === false, 'hk2 unregistered');

// ---- Single Instance ---------------------------------------------------------
assert(typeof bro.window.requestSingleInstance === 'function', 'requestSingleInstance exists');
assert(typeof bro.window.shutdownSingleInstance === 'function', 'shutdownSingleInstance exists');

let instanceArgsReceived = null;
let instanceCallCount = 0;

const singleOk = bro.window.requestSingleInstance({
    name: 'test-single-instance-app',
    onInstance: (args) => {
        instanceCallCount++;
        instanceArgsReceived = args;
    },
});
assert(singleOk === true, 'requestSingleInstance returns true');

// Simulate secondary instance message
const simInstOk = __bro_native.window.simulateSingleInstance('test-single-instance-app', '["--open", "file.txt"]');
assert(simInstOk === true, 'simulateSingleInstance returns true');
assert(instanceCallCount === 1, 'onInstance callback called once');
assert(Array.isArray(instanceArgsReceived), 'instanceArgsReceived is an array');
assert(instanceArgsReceived.length === 2, 'received 2 arguments');
assert(instanceArgsReceived[0] === '--open', 'arg 0 is --open');
assert(instanceArgsReceived[1] === 'file.txt', 'arg 1 is file.txt');

bro.window.shutdownSingleInstance();

console.log('bro.window desktop OK');
