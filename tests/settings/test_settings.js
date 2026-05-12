// Test bro.settings API — get/set, defaults, reset, action binding.
// Exercises src/js/settings_bindings.cpp and src/engine/settings.cpp.

assert(typeof bro === 'object', 'bro namespace exists');
assert(typeof bro.settings === 'object', 'bro.settings exists');

// =========================================================================
// get / set / setDefault
// =========================================================================

// Engine default values come back typed
const masterVol = bro.settings.get('audio.masterVolume');
assert(typeof masterVol === 'number', 'masterVolume is number, got ' + typeof masterVol);

const vsync = bro.settings.get('graphics.vsync');
assert(typeof vsync === 'boolean', 'vsync is boolean');

const width = bro.settings.get('graphics.width');
assert(typeof width === 'number', 'width is number');
assert(Number.isInteger(width), 'width is integer');

// Unknown key returns undefined
const unk = bro.settings.get('nonexistent.thing');
assert(unk === undefined || unk === '', 'unknown key returns undefined/empty');

// set a user override
bro.settings.set('audio.masterVolume', 0.5);
const got = bro.settings.get('audio.masterVolume');
assert(Math.abs(got - 0.5) < 0.001, 'set masterVolume = 0.5, got ' + got);

bro.settings.set('audio.muted', true);
assert(bro.settings.get('audio.muted') === true, 'set muted = true');

bro.settings.set('audio.muted', false);
assert(bro.settings.get('audio.muted') === false, 'unset muted');

// setDefault — lower priority than set
bro.settings.setDefault('audio.sfxVolume', 0.3);
assert(Math.abs(bro.settings.get('audio.sfxVolume') - 0.3) < 0.001, 'setDefault applied');

// User set overrides default
bro.settings.set('audio.sfxVolume', 0.8);
assert(Math.abs(bro.settings.get('audio.sfxVolume') - 0.8) < 0.001, 'set overrides default');

// =========================================================================
// getAll
// =========================================================================
const all = bro.settings.getAll();
assert(typeof all === 'object', 'getAll returns object');
assert(typeof all.graphics === 'object', 'getAll.graphics');
assert(typeof all.audio === 'object', 'getAll.audio');
assert(typeof all.input === 'object', 'getAll.input');

const audioCat = bro.settings.getAll('audio');
assert(typeof audioCat === 'object', 'getAll(audio) returns object');
assert(typeof audioCat.masterVolume === 'number', 'category has masterVolume');

// =========================================================================
// reset
// =========================================================================
bro.settings.set('audio.musicVolume', 0.2);
assert(Math.abs(bro.settings.get('audio.musicVolume') - 0.2) < 0.001, 'set musicVolume');
bro.settings.reset('audio');
// After reset, masterVolume should be back to default (1.0 if no app override)
const afterReset = bro.settings.get('audio.musicVolume');
assert(Math.abs(afterReset - 1.0) < 0.001 || afterReset !== 0.2,
       'reset reverts audio.musicVolume, got ' + afterReset);

// =========================================================================
// Action binding
// =========================================================================
bro.settings.defineAction('test_jump', [' ', 'ArrowUp']);
const keys = bro.settings.getActionKeys('test_jump');
assert(Array.isArray(keys), 'getActionKeys returns array');
assert(keys.length === 2, 'two keys bound');
assert(keys.indexOf(' ') !== -1, 'space is bound');
assert(keys.indexOf('ArrowUp') !== -1, 'ArrowUp is bound');

// rebind (user-level, persisted)
bro.settings.rebindAction('test_jump', [' ', 'w']);
const keys2 = bro.settings.getActionKeys('test_jump');
assert(keys2.indexOf('w') !== -1, 'rebind changed binding');
assert(keys2.indexOf('ArrowUp') === -1, 'rebind removed ArrowUp');

// reverse lookup
const act = bro.settings.getKeyAction(' ');
assert(act === 'test_jump', 'getKeyAction(space) = test_jump, got ' + act);
const act2 = bro.settings.getKeyAction('z');
assert(act2 === null || act2 === undefined || act2 === '',
       'unbound key returns null/empty, got ' + act2);

// getActions list
const actions = bro.settings.getActions();
assert(Array.isArray(actions), 'getActions returns array');
const found = actions.find(a => a.action === 'test_jump');
assert(found !== undefined, 'test_jump found in getActions');

// Reset single action
bro.settings.resetAction('test_jump');
const keys3 = bro.settings.getActionKeys('test_jump');
// After reset, should be back to defaults [' ', 'ArrowUp']
assert(keys3.indexOf('ArrowUp') !== -1, 'resetAction reverts to defaults');

// resetAllActions
bro.settings.rebindAction('test_jump', ['j']);
bro.settings.resetAllActions();
const keys4 = bro.settings.getActionKeys('test_jump');
assert(keys4.indexOf('ArrowUp') !== -1, 'resetAllActions reverts');

// =========================================================================
// Action events fire on bound key presses
// =========================================================================
bro.settings.defineAction('test_action', ['k']);
const fired = [];
const handler = (e) => fired.push({ action: e.detail.action, phase: e.detail.phase });
document.addEventListener('action', handler);

// SDL keycode 'k' = 107
keyDown(107);
keyUp(107);

assert(fired.length >= 2, 'action event fired down + up, got ' + fired.length);
const down = fired.find(f => f.phase === 'down');
const up = fired.find(f => f.phase === 'up');
assert(down && down.action === 'test_action', 'down phase fired');
assert(up && up.action === 'test_action', 'up phase fired');

document.removeEventListener('action', handler);

// =========================================================================
// getDisplayModes
// =========================================================================
const modes = bro.settings.getDisplayModes();
assert(Array.isArray(modes), 'getDisplayModes returns array');
// In headless mode, may be empty - just check it doesn't throw

// =========================================================================
// Cleanup - reset to defaults
// =========================================================================
bro.settings.reset();
