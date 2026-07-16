// Test the Gamepad API — virtual-pad injection through the engine layer:
// navigator.getGamepads() snapshot shape, gamepadconnected/disconnected window
// events, button press/release + analog triggers, stick axes, slot reuse,
// dual-rumble actuator, and bro.settings "gamepad:<name>" action bindings.
// Exercises src/engine/gamepad.cpp + src/js/gamepad_bindings.cpp.

// =========================================================================
// Baseline — API present, no pads connected
// =========================================================================

assert(typeof navigator.getGamepads === 'function', 'navigator.getGamepads exists');
let pads = navigator.getGamepads();
assert(Array.isArray(pads), 'getGamepads returns an array');
assert(pads.every((p) => p === null), 'no gamepads connected at start');

assert(typeof gamepadConnect === 'function', 'headless gamepadConnect exists');
assert(typeof gamepadDisconnect === 'function', 'headless gamepadDisconnect exists');
assert(typeof gamepadButton === 'function', 'headless gamepadButton exists');
assert(typeof gamepadAxis === 'function', 'headless gamepadAxis exists');

// =========================================================================
// Connect — event + snapshot shape
// =========================================================================

let connectedEvt = null;
window.addEventListener('gamepadconnected', (e) => { connectedEvt = e; });

const idx = gamepadConnect('Test Pad');
assert(typeof idx === 'number' && idx >= 0, 'gamepadConnect returns a slot index');

assert(connectedEvt !== null, 'gamepadconnected fired');
assert(connectedEvt.type === 'gamepadconnected', 'event type');
assert(connectedEvt.gamepad.index === idx, 'event gamepad.index matches slot');
assert(connectedEvt.gamepad.id === 'Test Pad', 'event gamepad.id');
assert(connectedEvt.gamepad.connected === true, 'event gamepad.connected');

pads = navigator.getGamepads();
const gp = pads[idx];
assert(gp !== null && typeof gp === 'object', 'connected pad appears in getGamepads');
assert(gp.id === 'Test Pad', 'snapshot id');
assert(gp.index === idx, 'snapshot index');
assert(gp.connected === true, 'snapshot connected');
assert(gp.mapping === 'standard', 'snapshot mapping is "standard"');
assert(typeof gp.timestamp === 'number', 'snapshot timestamp is a number');
assert(Array.isArray(gp.buttons) && gp.buttons.length === 17, '17 buttons');
assert(Array.isArray(gp.axes) && gp.axes.length === 4, '4 axes');
for (const b of gp.buttons) {
    assert(typeof b.pressed === 'boolean', 'button.pressed is boolean');
    assert(typeof b.touched === 'boolean', 'button.touched is boolean');
    assert(typeof b.value === 'number', 'button.value is number');
    assert(b.pressed === false && b.value === 0, 'buttons start released');
}
assert(gp.axes.every((a) => a === 0), 'axes start centered');

// =========================================================================
// Buttons — press/release reflected in fresh snapshots; old snapshots frozen
// =========================================================================

gamepadButton(idx, 'south', true);
const gpPressed = navigator.getGamepads()[idx];
assert(gpPressed.buttons[0].pressed === true, 'south press reflected (index 0)');
assert(gpPressed.buttons[0].value === 1, 'digital press has value 1');
assert(gpPressed.buttons[0].touched === true, 'pressed implies touched');
assert(gp.buttons[0].pressed === false, 'earlier snapshot is immutable');
assert(gpPressed.timestamp >= gp.timestamp, 'timestamp advances on state change');

gamepadButton(idx, 'south', false);
assert(navigator.getGamepads()[idx].buttons[0].pressed === false, 'south release reflected');

// Numeric button index + analog trigger value
gamepadButton(idx, 6, true, 0.75);
let trig = navigator.getGamepads()[idx].buttons[6];
assert(trig.pressed === true, 'trigger pressed at 0.75');
assert(Math.abs(trig.value - 0.75) < 1e-3, 'trigger analog value kept');

// Below the 0.1 threshold: value tracked, not pressed
gamepadButton(idx, 'lefttrigger', false, 0.05);
trig = navigator.getGamepads()[idx].buttons[6];
assert(trig.pressed === false, 'sub-threshold trigger not pressed');
assert(trig.touched === true, 'sub-threshold trigger still touched');
assert(Math.abs(trig.value - 0.05) < 1e-3, 'sub-threshold analog value kept');
gamepadButton(idx, 'lefttrigger', false, 0);

// =========================================================================
// Axes — by name and by index, clamped
// =========================================================================

gamepadAxis(idx, 'leftx', -0.5);
gamepadAxis(idx, 1, 1.0);
gamepadAxis(idx, 'righty', 5.0); // out of range -> clamped
const axes = navigator.getGamepads()[idx].axes;
assert(Math.abs(axes[0] + 0.5) < 1e-3, 'leftx set by name');
assert(Math.abs(axes[1] - 1.0) < 1e-3, 'lefty set by index');
assert(axes[3] === 1, 'axis value clamped to 1');
gamepadAxis(idx, 'leftx', 0);
gamepadAxis(idx, 1, 0);
gamepadAxis(idx, 'righty', 0);

// =========================================================================
// Action mapping — "gamepad:<name>" bindings dispatch "action" events
// =========================================================================

bro.settings.defineAction('test_jump', ['gamepad:south']);
assert(bro.settings.getKeyAction('gamepad:south') === 'test_jump',
       'gamepad binding resolves to action');

const actionLog = [];
document.body.addEventListener('action', (e) => {
    actionLog.push(e.detail.action + ':' + e.detail.phase + ':' + e.detail.key +
                   ':' + e.detail.gamepad);
});

gamepadButton(idx, 'south', true);
gamepadButton(idx, 'south', false);
assert(actionLog.length === 2, 'press+release dispatched two action events, got ' + actionLog.length);
assert(actionLog[0] === 'test_jump:down:gamepad:south:' + idx, 'down phase payload: ' + actionLog[0]);
assert(actionLog[1] === 'test_jump:up:gamepad:south:' + idx, 'up phase payload: ' + actionLog[1]);

// Analog trigger crossing the threshold fires an edge; wiggling within one
// side of it doesn't.
bro.settings.defineAction('test_boost', ['gamepad:righttrigger']);
actionLog.length = 0;
gamepadButton(idx, 'righttrigger', false, 0.05); // below threshold: no edge
gamepadButton(idx, 'righttrigger', true, 0.9);   // crosses up: down edge
gamepadButton(idx, 'righttrigger', true, 0.6);   // still pressed: no edge
gamepadButton(idx, 'righttrigger', false, 0.0);  // crosses down: up edge
assert(actionLog.length === 2, 'trigger threshold crossings fire exactly two events, got ' + actionLog.length);
assert(actionLog[0].startsWith('test_boost:down'), 'trigger down edge');
assert(actionLog[1].startsWith('test_boost:up'), 'trigger up edge');

// Rebinding moves the action to the new button (user layer, then cleaned up).
bro.settings.rebindAction('test_jump', ['gamepad:start']);
actionLog.length = 0;
gamepadButton(idx, 'south', true);
gamepadButton(idx, 'south', false);
assert(actionLog.length === 0, 'unbound button no longer fires');
gamepadButton(idx, 'start', true);
gamepadButton(idx, 'start', false);
assert(actionLog.length === 2 && actionLog[0].startsWith('test_jump:down:gamepad:start'),
       'rebound button fires the action');
bro.settings.resetAction('test_jump'); // drop the persisted user rebind

// =========================================================================
// Rumble — vibrationActuator shape + playEffect/reset promises
// =========================================================================

const pad = navigator.getGamepads()[idx];
assert(typeof pad.vibrationActuator === 'object', 'vibrationActuator exists');
assert(pad.vibrationActuator.type === 'dual-rumble', 'actuator type');

let effectResult = null;
pad.vibrationActuator.playEffect('dual-rumble', {
    duration: 100, strongMagnitude: 1.0, weakMagnitude: 0.5,
}).then((r) => { effectResult = r; });
flush(); // run microtasks
assert(effectResult === 'complete', 'playEffect resolves "complete", got ' + effectResult);

let resetResult = null;
pad.vibrationActuator.reset().then((r) => { resetResult = r; });
flush();
assert(resetResult === 'complete', 'reset resolves "complete"');

// =========================================================================
// Disconnect — event, null slot, slot reuse
// =========================================================================

let disconnectedEvt = null;
window.addEventListener('gamepaddisconnected', (e) => { disconnectedEvt = e; });

gamepadDisconnect(idx);
assert(disconnectedEvt !== null, 'gamepaddisconnected fired');
assert(disconnectedEvt.gamepad.index === idx, 'disconnect event carries the slot');
assert(disconnectedEvt.gamepad.connected === false, 'disconnect snapshot has connected=false');
assert(navigator.getGamepads()[idx] === null, 'disconnected slot reads null');

// The vacated slot is reused by the next arrival; a second pad coexists.
const idx2 = gamepadConnect('Pad A');
assert(idx2 === idx, 'freed slot is reused');
const idx3 = gamepadConnect('Pad B');
assert(idx3 !== idx2, 'second pad gets its own slot');
pads = navigator.getGamepads();
assert(pads[idx2].id === 'Pad A' && pads[idx3].id === 'Pad B', 'both pads polled independently');
gamepadDisconnect(idx2);
gamepadDisconnect(idx3);
assert(navigator.getGamepads().every((p) => p === null), 'all pads disconnected');

console.log('gamepad tests passed');
