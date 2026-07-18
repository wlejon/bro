// Test the extended action-binding vocabulary + polled action state:
//   "mouse:<button>" bindings (left/middle/right/x1/x2),
//   "gamepad:<axis>+/-" axis-direction bindings with deadzone + hysteresis,
//   bro.settings.getActionStrength() / isActionPressed(),
//   detail.strength on "action" events.
// Exercises src/engine/action_input.cpp + settings/gamepad/input_handling
// integration through the headless mouse/keyboard/gamepad injection seams.

const EPS = 1e-4;

const log = [];
const handler = (e) => log.push({
    action: e.detail.action,
    phase: e.detail.phase,
    key: e.detail.key,
    strength: e.detail.strength,
    gamepad: e.detail.gamepad,
});
document.addEventListener('action', handler);

// =========================================================================
// Mouse-button bindings
// =========================================================================

bro.settings.defineAction('act_fire', ['mouse:left']);
assert(bro.settings.getKeyAction('mouse:left') === 'act_fire',
       'mouse binding resolves to action');

assert(bro.settings.isActionPressed('act_fire') === false, 'not pressed at rest');
assert(bro.settings.getActionStrength('act_fire') === 0, 'strength 0 at rest');

mouseDown(10, 10);
assert(log.length === 1, 'mousedown fired one action event, got ' + log.length);
assert(log[0].action === 'act_fire' && log[0].phase === 'down', 'down phase');
assert(log[0].key === 'mouse:left', 'detail.key is the binding string');
assert(log[0].strength === 1, 'mouse down strength 1');
assert(bro.settings.isActionPressed('act_fire') === true, 'polled pressed while held');
assert(bro.settings.getActionStrength('act_fire') === 1, 'polled strength 1 while held');

mouseUp(10, 10);
assert(log.length === 2, 'mouseup fired the up event');
assert(log[1].phase === 'up' && log[1].strength === 0, 'up phase, strength 0');
assert(bro.settings.isActionPressed('act_fire') === false, 'released after up');

// Unbound buttons don't fire; x1/x2 names work (DOM buttons 3/4).
log.length = 0;
mouseDown(10, 10, 2);   // right button — not bound
mouseUp(10, 10, 2);
assert(log.length === 0, 'unbound mouse button fires nothing');

bro.settings.defineAction('act_back', ['mouse:x1']);
bro.settings.defineAction('act_fwd', ['mouse:x2']);
mouseDown(10, 10, 3); mouseUp(10, 10, 3);
mouseDown(10, 10, 4); mouseUp(10, 10, 4);
assert(log.length === 4, 'x1 + x2 each fired down/up, got ' + log.length);
assert(log[0].action === 'act_back' && log[0].key === 'mouse:x1', 'x1 payload');
assert(log[2].action === 'act_fwd' && log[2].key === 'mouse:x2', 'x2 payload');

// Rebind moves the action to another button (user-level, persisted).
bro.settings.rebindAction('act_fire', ['mouse:middle']);
log.length = 0;
mouseDown(10, 10); mouseUp(10, 10);          // left no longer bound
assert(log.length === 0, 'old button unbound after rebind');
mouseDown(10, 10, 1);
assert(log.length === 1 && log[0].action === 'act_fire' && log[0].key === 'mouse:middle',
       'rebound to middle button');
mouseUp(10, 10, 1);
bro.settings.resetAction('act_fire');

// =========================================================================
// Keyboard contributes to polled state (strength 0/1, detail.strength)
// =========================================================================

bro.settings.defineAction('act_kb', ['k']);
log.length = 0;
keyDown(107);
assert(log.length === 1 && log[0].strength === 1, 'keyboard down strength 1');
assert(bro.settings.isActionPressed('act_kb') === true, 'keyboard polled pressed');
assert(bro.settings.getActionStrength('act_kb') === 1, 'keyboard polled strength');
keyUp(107);
assert(log[1].phase === 'up' && log[1].strength === 0, 'keyboard up strength 0');
assert(bro.settings.isActionPressed('act_kb') === false, 'keyboard released');

// =========================================================================
// Gamepad axis-direction bindings — deadzone, rescale, hysteresis
// =========================================================================

const idx = gamepadConnect('Action Test Pad');

bro.settings.defineAction('act_right', ['gamepad:leftx+']);
bro.settings.defineAction('act_left', ['gamepad:leftx-']);

log.length = 0;
gamepadAxis(idx, 'leftx', 0.5);
assert(log.length === 1, 'axis past deadzone fired one event, got ' + log.length);
assert(log[0].action === 'act_right' && log[0].phase === 'down', 'axis+ down');
assert(log[0].key === 'gamepad:leftx+', 'axis binding string in detail.key');
assert(log[0].gamepad === idx, 'detail.gamepad is the pad slot');
// Deadzone-rescaled: (0.5 - 0.1) / (1 - 0.1) = 0.4444...
assert(Math.abs(log[0].strength - 0.44444) < 1e-3,
       'axis down strength rescaled, got ' + log[0].strength);
assert(bro.settings.isActionPressed('act_right') === true, 'axis polled pressed');
assert(Math.abs(bro.settings.getActionStrength('act_right') - 0.44444) < 1e-3,
       'axis polled strength matches');
assert(bro.settings.isActionPressed('act_left') === false, 'opposite direction untouched');
assert(bro.settings.getActionStrength('act_left') === 0, 'opposite strength 0');

// Full deflection -> strength 1.
gamepadAxis(idx, 'leftx', 1.0);
assert(Math.abs(bro.settings.getActionStrength('act_right') - 1) < EPS,
       'full deflection strength 1');
assert(log.length === 1, 'analog change past the edge fires no extra event');

// Hysteresis: press at >= 0.1, release only below 0.075 (0.1 * 0.75).
gamepadAxis(idx, 'leftx', 0.09);
assert(log.length === 1, 'inside hysteresis band: still pressed, no event');
assert(bro.settings.isActionPressed('act_right') === true, 'latched through the band');
gamepadAxis(idx, 'leftx', 0.05);
assert(log.length === 2, 'below release threshold fired the up');
assert(log[1].phase === 'up' && log[1].strength === 0, 'axis up strength 0');
assert(bro.settings.isActionPressed('act_right') === false, 'axis released');

// Negative direction.
log.length = 0;
gamepadAxis(idx, 'leftx', -0.6);
assert(log.length === 1 && log[0].action === 'act_left' && log[0].phase === 'down',
       'negative direction fires its own action');
assert(Math.abs(log[0].strength - (0.6 - 0.1) / 0.9) < 1e-3, 'negative rescale');
gamepadAxis(idx, 'leftx', 0);
assert(log[1].phase === 'up', 'centering releases');

// Per-action deadzone option.
bro.settings.defineAction('act_dz', ['gamepad:rightx+'], { deadzone: 0.5 });
log.length = 0;
gamepadAxis(idx, 'rightx', 0.4);
assert(log.length === 0, 'below custom deadzone: no press');
assert(bro.settings.isActionPressed('act_dz') === false, 'polled agrees');
gamepadAxis(idx, 'rightx', 0.6);
assert(log.length === 1 && log[0].phase === 'down', 'past custom deadzone: down');
// (0.6 - 0.5) / (1 - 0.5) = 0.2
assert(Math.abs(log[0].strength - 0.2) < 1e-3, 'custom-deadzone rescale');
// Release margin scales with the deadzone: 0.5 * 0.75 = 0.375.
gamepadAxis(idx, 'rightx', 0.4);
assert(log.length === 1, 'inside scaled hysteresis band: still pressed');
gamepadAxis(idx, 'rightx', 0.3);
assert(log.length === 2 && log[1].phase === 'up', 'below scaled release: up');
gamepadAxis(idx, 'rightx', 0);

// =========================================================================
// Trigger bindings contribute their analog value to strength
// =========================================================================

bro.settings.defineAction('act_trig', ['gamepad:righttrigger']);
log.length = 0;
gamepadButton(idx, 'righttrigger', true, 0.75);
assert(log.length === 1 && log[0].phase === 'down', 'trigger edge fired');
assert(Math.abs(log[0].strength - 0.75) < 1e-3, 'trigger event strength is analog');
assert(Math.abs(bro.settings.getActionStrength('act_trig') - 0.75) < 1e-3,
       'trigger polled strength is analog');
gamepadButton(idx, 'righttrigger', true, 0.9);
assert(Math.abs(bro.settings.getActionStrength('act_trig') - 0.9) < 1e-3,
       'polled strength tracks analog changes without new edges');
gamepadButton(idx, 'righttrigger', false, 0);
assert(bro.settings.getActionStrength('act_trig') === 0, 'trigger released');

// =========================================================================
// Mixed bindings: strength is the max across all of an action's bindings
// =========================================================================

bro.settings.defineAction('act_mix', ['k', 'gamepad:lefty-']);
gamepadAxis(idx, 'lefty', -0.55);   // rescaled: (0.55-0.1)/0.9 = 0.5
assert(Math.abs(bro.settings.getActionStrength('act_mix') - 0.5) < 1e-3,
       'axis contribution');
keyDown(107);
assert(bro.settings.getActionStrength('act_mix') === 1, 'key held: max wins');
keyUp(107);
assert(Math.abs(bro.settings.getActionStrength('act_mix') - 0.5) < 1e-3,
       'back to axis contribution after key release');
gamepadAxis(idx, 'lefty', 0);

// Unknown action: safe defaults.
assert(bro.settings.getActionStrength('act_nope') === 0, 'unknown action strength 0');
assert(bro.settings.isActionPressed('act_nope') === false, 'unknown action not pressed');

// =========================================================================
// Cleanup
// =========================================================================
document.removeEventListener('action', handler);
gamepadDisconnect(idx);
bro.settings.resetAllActions();
bro.settings.reset();
