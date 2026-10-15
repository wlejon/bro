// The Windows desktop shell's keyboard hook decision (platform::desktop::
// shellHookAction), driven through the hotkey router a real hook uses:
// __bro_native.window.simulateShellHookKey(key, mods, code, down, repeat,
// shellForeground) returns the routing bits 1 consumed, 2 fired, 4 grabbed
// plus 8 suppressed, 16 mask armed, 32 release replayed behind the mask,
// 64 forwarded to the document. No hook is installed here.

const CONSUMED = 1, FIRED = 2, GRABBED = 4, SUPPRESS = 8, MASK = 16, REPLAY = 32, FORWARD = 64;
const key = (k, mods, code, down, opts = {}) =>
    __bro_native.window.simulateShellHookKey(k, mods || '', code, down, !!opts.repeat, !!opts.foreground);

assert(bro.window.hotkeyRouting === 'none', 'headless routes no hotkeys itself: ' + bro.window.hotkeyRouting);

bro.window.unregisterAllGlobalHotkeys();
__bro_native.window.resetHotkeyKeys();

const fired = [];
bro.window.registerGlobalHotkey('Super', () => fired.push('tap'));
bro.window.registerGlobalHotkey('Super+V', () => fired.push('clipboard'));
bro.window.registerGlobalHotkey('Ctrl+Space', () => fired.push('launcher'));
bro.window.registerGlobalHotkey('Alt+Space', () => fired.push('launcher-alt'));
bro.window.registerGlobalHotkey('Alt+Tab', () => fired.push('next'), { grab: true });

const SUPER = 125, V = 47, E = 18, CTRL = 29, SPACE = 57, ALT = 56, TAB = 15, LEFT = 105, A = 30;

// ---- a lone Super tap: fires; its release is the shell's, replayed behind the mask
assert(key('super', 'super', SUPER, true) === 0, 'Super press passes');
assert(key('super', '', SUPER, false) === (FIRED | SUPPRESS | REPLAY), 'tap release: fired, swallowed, replayed');
assert(fired.join() === 'tap', 'tap fired: ' + fired);
fired.length = 0;

// ---- a bound Win chord: its key never reaches anyone; the Win release is not a tap
assert(key('super', 'super', SUPER, true) === 0, 'Super press');
assert(key('v', 'super', V, true) === (CONSUMED | FIRED | SUPPRESS | MASK), 'Super+V swallowed, mask armed');
assert(key('v', 'super', V, true, { repeat: true }) === (CONSUMED | SUPPRESS | MASK), 'its repeat swallowed');
assert(key('v', 'super', V, false) === (CONSUMED | SUPPRESS), 'its release swallowed');
assert(key('super', '', SUPER, false) === (SUPPRESS | REPLAY), 'Win release replayed behind the mask, no tap');
assert(fired.join() === 'clipboard', 'only the chord fired: ' + fired);
fired.length = 0;

// ---- an unbound Win chord passes untouched, and so does its Win release
assert(key('super', 'super', SUPER, true) === 0, 'Super press');
assert(key('e', 'super', E, true) === 0, 'Super+E passes');
assert(key('e', 'super', E, false) === 0, 'its release passes');
assert(key('super', '', SUPER, false) === 0, 'Win release passes (the system keeps its own Win+E)');
assert(fired.length === 0, 'nothing fired');

// ---- the mask only follows a swallowed key: the next unbound Win use is clean again
key('super', 'super', SUPER, true);
key('v', 'super', V, true);
key('v', 'super', V, false);
key('super', '', SUPER, false);
fired.length = 0;
key('super', 'super', SUPER, true);
assert(key('e', 'super', E, true) === 0 && key('e', 'super', E, false) === 0, 'Super+E after a chord passes');
assert(key('super', '', SUPER, false) === 0, 'and its Win release is not replayed');

// ---- Ctrl chords: swallowed, no mask (a Ctrl release means nothing alone)
key('ctrl', 'ctrl', CTRL, true);
assert(key('space', 'ctrl', SPACE, true) === (CONSUMED | FIRED | SUPPRESS), 'Ctrl+Space swallowed, no mask');
key('space', 'ctrl', SPACE, false);
assert(key('ctrl', '', CTRL, false) === 0, 'Ctrl release passes');
// Alt chords mask the Alt release (it would open the focused window's menu bar)
key('alt', 'alt', ALT, true);
assert(key('space', 'alt', SPACE, true) === (CONSUMED | FIRED | SUPPRESS | MASK), 'Alt+Space swallowed, masked');
key('space', 'alt', SPACE, false);
assert(key('alt', '', ALT, false) === (SUPPRESS | REPLAY), 'Alt release replayed behind the mask');
assert(fired.join() === 'launcher,launcher-alt', 'launchers fired: ' + fired);
fired.length = 0;

// ---- a grab while another app has the foreground: its keys go to the shell's document
assert(key('alt', 'alt', ALT, true) === 0, 'Alt press');
assert(key('tab', 'alt', TAB, true) === (CONSUMED | FIRED | GRABBED | SUPPRESS | MASK), 'Alt+Tab grabs');
key('tab', 'alt', TAB, false);
assert(key('left', 'alt', LEFT, true) === (GRABBED | SUPPRESS | MASK | FORWARD), 'arrow forwarded, not the app\'s');
assert(key('left', 'alt', LEFT, false) === (GRABBED | SUPPRESS | FORWARD), 'its release too');
assert(key('alt', '', ALT, false) === (GRABBED | SUPPRESS | REPLAY | FORWARD),
       'the Alt release ends the grab: forwarded, and replayed for the system behind the mask');
assert(key('a', '', A, true) === 0 && key('a', '', A, false) === 0, 'grab over');
fired.length = 0;

// ---- the same grab while the shell has the foreground: its keys reach it the ordinary way
key('alt', 'alt', ALT, true);
key('tab', 'alt', TAB, true, { foreground: true });
key('tab', 'alt', TAB, false, { foreground: true });
assert(key('left', 'alt', LEFT, true, { foreground: true }) === GRABBED, 'nothing to forward');
key('left', 'alt', LEFT, false, { foreground: true });
key('alt', '', ALT, false, { foreground: true });

bro.window.unregisterAllGlobalHotkeys();
__bro_native.window.resetHotkeyKeys();
