// Media and volume keys carry their UI Events names: e.key "MediaPlayPause",
// "MediaTrackNext", ... and the matching e.code, where they once arrived as
// a number (e.key "1073742095") and "Unknown271". Headless keyDown/keyUp take
// those names as well as keycodes.

const keys = [];
document.addEventListener('keydown', function (e) {
    keys.push({ key: e.key, code: e.code, keyCode: e.keyCode });
});

// By scancode (pins the platform -> event tables): SDL's consumer-page and
// volume scancodes.
const SC = 0x40000000;
const cases = [
    [262, 'MediaPlay',          'MediaPlay'],
    [263, 'MediaPause',         'MediaPause'],
    [267, 'MediaTrackNext',     'MediaTrackNext'],
    [268, 'MediaTrackPrevious', 'MediaTrackPrevious'],
    [269, 'MediaStop',          'MediaStop'],
    [271, 'MediaPlayPause',     'MediaPlayPause'],
    [127, 'AudioVolumeMute',    'AudioVolumeMute'],
    [128, 'AudioVolumeUp',      'AudioVolumeUp'],
    [129, 'AudioVolumeDown',    'AudioVolumeDown'],
];
for (const [sc] of cases) { keyDown(SC | sc, sc); keyUp(SC | sc, sc); }
flush();
assert(keys.length === cases.length, 'one keydown per media key, got ' + keys.length);
cases.forEach(function (c, i) {
    const k = keys[i] || {};
    assert(k.key === c[1], 'scancode ' + c[0] + ' key is ' + c[1] + ', got ' + k.key);
    assert(k.code === c[2], 'scancode ' + c[0] + ' code is ' + c[2] + ', got ' + k.code);
});
// Legacy keyCode as Chromium on Windows reports these keys.
assert(keys[2].keyCode === 176, 'MediaTrackNext keyCode 176, got ' + keys[2].keyCode);
assert(keys[5].keyCode === 179, 'MediaPlayPause keyCode 179, got ' + keys[5].keyCode);
assert(keys[7].keyCode === 175, 'AudioVolumeUp keyCode 175, got ' + keys[7].keyCode);

// By name: key names, code names and characters.
keys.length = 0;
const names = [
    ['MediaPlayPause', 'MediaPlayPause', 'MediaPlayPause'],
    ['MediaTrackNext', 'MediaTrackNext', 'MediaTrackNext'],
    ['MediaTrackPrevious', 'MediaTrackPrevious', 'MediaTrackPrevious'],
    ['MediaStop', 'MediaStop', 'MediaStop'],
    ['AudioVolumeMute', 'AudioVolumeMute', 'AudioVolumeMute'],
    ['Enter', 'Enter', 'Enter'],
    ['ArrowLeft', 'ArrowLeft', 'ArrowLeft'],
    ['KeyA', 'a', 'KeyA'],
    ['b', 'b', 'KeyB'],
];
for (const [name] of names) { keyDown(name); keyUp(name); }
flush();
assert(keys.length === names.length, 'one keydown per named key, got ' + keys.length);
names.forEach(function (c, i) {
    const k = keys[i] || {};
    assert(k.key === c[1], 'keyDown("' + c[0] + '") key is ' + c[1] + ', got ' + k.key);
    assert(k.code === c[2], 'keyDown("' + c[0] + '") code is ' + c[2] + ', got ' + k.code);
});

let threw = false;
try { keyDown('NoSuchKey'); } catch (e) { threw = true; }
assert(threw, 'keyDown of an unknown name throws');
