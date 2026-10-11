// Global hotkeys as a host that owns the keyboard (DRM) routes them:
// __bro_native.window.simulateHotkeyKey(key, mods, code, down, repeat) feeds
// one key event through the same router and returns bits
// 1 consumed, 2 fired, 4 grabbed. Plus per-id callback dispatch and
// bro.window.displayMode.

const CONSUMED = 1, FIRED = 2, GRABBED = 4;
const key = (k, mods, code, down, repeat) =>
    __bro_native.window.simulateHotkeyKey(k, mods || '', code, down, !!repeat);

assert(bro.window.displayMode === 'headless', 'displayMode is headless here: ' + bro.window.displayMode);

bro.window.unregisterAllGlobalHotkeys();
__bro_native.window.resetHotkeyKeys();

const fired = [];
const idL = bro.window.registerGlobalHotkey('Super+L', (e) => fired.push(['lock', e.id, e.accelerator]));
const idSpace = bro.window.registerGlobalHotkey('Ctrl+Space', () => fired.push(['launcher']));
const idTap = bro.window.registerGlobalHotkey('Super', () => fired.push(['tap']));
const idTab = bro.window.registerGlobalHotkey('Alt+Tab', () => fired.push(['next']), { grab: true });
const idBack = bro.window.registerGlobalHotkey('Alt+Shift+Tab', () => fired.push(['prev']), { grab: true });
const idVol = bro.window.registerGlobalHotkey('VolumeUp', () => fired.push(['vol']));
assert(idL > 0 && idSpace > 0 && idTap > 0 && idTab > 0 && idBack > 0 && idVol > 0, 'registered');

// Only the matching callback runs (not every registered one).
assert(__bro_native.window.simulateGlobalHotkey('Super+L') === true, 'simulate by accelerator');
assert(fired.length === 1 && fired[0][0] === 'lock' && fired[0][1] === idL, 'per-id dispatch: ' + JSON.stringify(fired));
fired.length = 0;

// ---- a chord: consumed, callback once, repeat and release consumed too ----
const SUPER = 125, L = 38, CTRL = 29, SPACE = 57, ALT = 56, TAB = 15, SHIFT = 42, A = 30;
assert(key('super', 'super', SUPER, true) === 0, 'Super press passes through (modifiers are never consumed)');
assert(key('l', 'super', L, true) === (CONSUMED | FIRED), 'Super+L consumed and fired');
assert(fired.length === 1 && fired[0][0] === 'lock', 'lock fired');
assert(key('l', 'super', L, true, true) === CONSUMED, 'its auto-repeat is consumed, not refired');
assert(key('l', 'super', L, false) === CONSUMED, 'its release is consumed');
assert(key('super', '', SUPER, false) === 0, 'Super release after a chord is not a tap');
assert(fired.length === 1, 'no tap fired after Super+L');
fired.length = 0;

// ---- a tap: Super pressed and released alone -----------------------------
assert(key('super', 'super', SUPER, true) === 0, 'tap press');
assert(key('super', '', SUPER, false) === FIRED, 'tap fires on release, not consumed');
assert(fired.length === 1 && fired[0][0] === 'tap', 'tap fired');
fired.length = 0;

// Another key in between cancels the tap; so does a pointer press (host side).
key('super', 'super', SUPER, true);
assert(key('a', 'super', A, true) === 0, 'Super+A is not registered: passes through');
key('a', 'super', A, false);
assert(key('super', '', SUPER, false) === 0, 'no tap after Super+A');
assert(fired.length === 0, 'nothing fired');

// ---- unregistered keys pass ----------------------------------------------
assert(key('a', '', A, true) === 0 && key('a', '', A, false) === 0, 'plain key passes');
key('ctrl', 'ctrl', CTRL, true);
assert(key('space', 'ctrl', SPACE, true) === (CONSUMED | FIRED), 'Ctrl+Space consumed');
key('space', 'ctrl', SPACE, false);
key('ctrl', '', CTRL, false);
assert(fired.length === 1 && fired[0][0] === 'launcher', 'launcher fired');
fired.length = 0;
assert(key('volumeup', '', 115, true) === (CONSUMED | FIRED) && fired[0][0] === 'vol', 'media key chord');
key('volumeup', '', 115, false);
fired.length = 0;

// ---- a grab: Alt+Tab holds the keyboard until Alt is released -------------
assert(key('alt', 'alt', ALT, true) === 0, 'Alt press: no grab yet');
assert(key('tab', 'alt', TAB, true) === (CONSUMED | FIRED | GRABBED), 'Alt+Tab fires and grabs');
assert(key('tab', 'alt', TAB, false) === (CONSUMED | GRABBED), 'Tab release consumed, still grabbed');
assert(key('tab', 'alt', TAB, true) === (CONSUMED | FIRED | GRABBED), 'Tab again fires again');
key('tab', 'alt', TAB, false);
assert(key('shift', 'alt+shift', SHIFT, true) === GRABBED, 'Shift during the grab goes to the shell');
assert(key('tab', 'alt+shift', TAB, true) === (CONSUMED | FIRED | GRABBED), 'Alt+Shift+Tab fires');
key('tab', 'alt+shift', TAB, false);
key('shift', 'alt', SHIFT, false);
assert(key('a', 'alt', A, true) === GRABBED, 'other keys go to the shell while grabbed');
key('a', 'alt', A, false);
assert(key('alt', '', ALT, false) === GRABBED, 'the Alt release that ends the grab is still grabbed');
assert(key('a', '', A, true) === 0, 'grab over');
key('a', '', A, false);
assert(JSON.stringify(fired.map((f) => f[0])) === '["next","next","prev"]', 'switcher sequence: ' + JSON.stringify(fired));
fired.length = 0;

// ---- unregistering stops matching ----------------------------------------
assert(bro.window.unregisterGlobalHotkey(idL) === true, 'unregister');
key('super', 'super', SUPER, true);
assert(key('l', 'super', L, true) === 0, 'Super+L passes once unregistered');
key('l', 'super', L, false);
key('super', '', SUPER, false);
fired.length = 0;

bro.window.unregisterAllGlobalHotkeys();
__bro_native.window.resetHotkeyKeys();
