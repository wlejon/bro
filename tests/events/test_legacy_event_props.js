// Legacy `keyCode` / `which` on keyboard and mouse events.
//
// Both are deprecated and both are still what a great deal of shipped library
// code reads. bro reported a constant 0 for keyCode/which on keyboard events
// and had no `which` on mouse events at all, so every binding table keyed on
// them collapsed onto one non-key: CodeMirror decides a press is a left click
// with `e.which == 1` and ignores anything else, then resolves each keystroke
// through `keyNames[e.keyCode]`. Neither worked.

const root = document.getElementById('root');
root.innerHTML =
    '<input id="field" style="position:absolute;left:0;top:0;width:200px;height:24px">' +
    '<div id="pad" style="position:absolute;left:0;top:40px;width:200px;height:60px"></div>';
flush();

const field = document.getElementById('field');
const pad = document.getElementById('pad');

// ---------------------------------------------------------------- keyboard
// On the document: Escape blurs the field, so later keys legitimately land on
// the body — the target is not what this test is about.
const keys = [];
document.addEventListener('keydown', function (e) {
    keys.push({ key: e.key, code: e.code, keyCode: e.keyCode, which: e.which,
                charCode: e.charCode });
});

const fr = field.getBoundingClientRect();
click((fr.left + fr.width / 2) | 0, (fr.top + fr.height / 2) | 0);
flush();
assert(document.activeElement === field, 'field is focused');

// SDL keycodes.
const SDLK_a = 0x61, SDLK_z = 0x7a, SDLK_1 = 0x31;
const SDLK_RETURN = 0x0d, SDLK_BACKSPACE = 0x08, SDLK_ESCAPE = 0x1b;
const SDLK_LEFT = 0x40000050, SDLK_SPACE = 0x20;

[SDLK_a, SDLK_z, SDLK_1, SDLK_RETURN, SDLK_BACKSPACE, SDLK_ESCAPE, SDLK_LEFT, SDLK_SPACE]
    .forEach(function (k) { keyDown(k); keyUp(k); });
flush();

function seen(keyName) {
    for (const k of keys) if (k.key === keyName || k.code === keyName) return k;
    return null;
}
function expect(keyName, want) {
    const k = seen(keyName);
    assert(k !== null, 'saw a keydown for ' + keyName);
    assert(k.keyCode === want,
           keyName + ' keyCode is ' + want + ', got ' + k.keyCode +
           ' (key="' + k.key + '" code="' + k.code + '")');
    assert(k.which === k.keyCode, keyName + ' which matches keyCode');
    assert(k.charCode === 0, keyName + ' charCode stays 0 (keypress is not fired)');
}

expect('a', 65);              // letters report the UPPERCASE code
expect('z', 90);
expect('1', 49);
expect('Enter', 13);
expect('Backspace', 8);
expect('Escape', 27);
expect('ArrowLeft', 37);
expect(' ', 32);

// The lookup a library actually performs.
const NAMES = { 13: 'Enter', 8: 'Backspace', 27: 'Esc', 37: 'Left', 32: 'Space' };
const resolved = keys.map(k => NAMES[k.keyCode]).filter(Boolean);
assert(resolved.indexOf('Enter') !== -1 && resolved.indexOf('Left') !== -1,
       'a keyCode-keyed binding table resolves real names, got ' + JSON.stringify(resolved));

// ------------------------------------------------------------------- mouse
const buttons = [];
pad.addEventListener('mousedown', function (e) {
    buttons.push({ button: e.button, which: e.which });
});

const pr = pad.getBoundingClientRect();
const px = (pr.left + pr.width / 2) | 0, py = (pr.top + pr.height / 2) | 0;
click(px, py, 0);
click(px, py, 1);
click(px, py, 2);
flush();

assert(buttons.length === 3, 'three presses, got ' + buttons.length);
assert(buttons[0].button === 0 && buttons[0].which === 1, 'left: button 0, which 1');
assert(buttons[1].button === 1 && buttons[1].which === 2, 'middle: button 1, which 2');
assert(buttons[2].button === 2 && buttons[2].which === 3, 'right: button 2, which 3');

// The guard the widgets write.
assert(buttons.filter(b => b.which === 1).length === 1,
       'exactly one press reads as a left click');

root.innerHTML = '';
console.log('PASS: legacy keyCode / which on keyboard and mouse events');
