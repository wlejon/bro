// KeyboardEvent.code for the modifier keys and the numpad.
//
// The scancode table had the modifier block shifted by one: SDL (= USB HID)
// orders it LCTRL 224, LSHIFT 225, LALT 226, LGUI 227, then the right-hand
// four, but bro reported Left Shift as "ShiftRight", Left Control as
// "ShiftLeft", Left Alt as "ControlLeft" and Right Control as "AltLeft", and
// had no entry for GUI at all — so on macOS every Command press arrived as
// code "Unknown227". The numpad had no entries either.

const keys = [];
document.addEventListener('keydown', function (e) {
    keys.push({ key: e.key, code: e.code, keyCode: e.keyCode });
});

// keyDown(keycode, scancode): pass the scancode explicitly so the test pins
// the scancode -> code table, not SDL's keycode -> scancode lookup.
const cases = [
    [0x400000e0, 224, 'Control', 'ControlLeft'],
    [0x400000e1, 225, 'Shift',   'ShiftLeft'],
    [0x400000e2, 226, 'Alt',     'AltLeft'],
    [0x400000e3, 227, 'Meta',    'MetaLeft'],
    [0x400000e4, 228, 'Control', 'ControlRight'],
    [0x400000e5, 229, 'Shift',   'ShiftRight'],
    [0x400000e6, 230, 'Alt',     'AltRight'],
    [0x400000e7, 231, 'Meta',    'MetaRight'],
];
for (const [kc, sc] of cases) { keyDown(kc, sc); keyUp(kc, sc); }
flush();

assert(keys.length === cases.length,
       'one keydown per modifier, got ' + keys.length);
cases.forEach(function (c, i) {
    const k = keys[i];
    assert(k.key === c[2], 'scancode ' + c[1] + ' key is ' + c[2] + ', got ' + k.key);
    assert(k.code === c[3], 'scancode ' + c[1] + ' code is ' + c[3] + ', got ' + k.code);
});
// Legacy keyCode for Command / the Windows key, as browsers report it.
assert(keys[3].keyCode === 91, 'MetaLeft keyCode is 91, got ' + keys[3].keyCode);
assert(keys[7].keyCode === 93, 'MetaRight keyCode is 93, got ' + keys[7].keyCode);

// Numpad and the extended function keys. `key` is the character the keypad
// types (it used to be the decimal SDL keycode, e.g. "1073741913").
keys.length = 0;
const more = [
    [0x40000059, 89,  'Numpad1',     '1',     97],
    [0x40000062, 98,  'Numpad0',     '0',     96],
    [0x40000057, 87,  'NumpadAdd',   '+',     null],
    [0x40000058, 88,  'NumpadEnter', 'Enter', 13],
    [0x40000068, 104, 'F13',         null,    null],
];
for (const [kc, sc] of more) { keyDown(kc, sc); keyUp(kc, sc); }
flush();
more.forEach(function (c, i) {
    const k = keys[i];
    assert(k && k.code === c[2], 'scancode ' + c[1] + ' code is ' + c[2] +
           ', got ' + (k && k.code));
    if (c[3] !== null) assert(k && k.key === c[3], c[2] + ' key is ' + c[3] + ', got ' + (k && k.key));
    if (c[4] !== null) assert(k && k.keyCode === c[4], c[2] + ' keyCode is ' + c[4] + ', got ' + (k && k.keyCode));
});

console.log('PASS: modifier and numpad KeyboardEvent.code');
