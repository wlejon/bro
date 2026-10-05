// <terminal>: keys reach the child byte for byte.
//
// bropty's pty_child (`keys N`) switches its tty to raw mode, prints READY
// and then the hex of the next N bytes it reads. Keys go through the same
// engine path as a real window (headless keyDown / textInput / keyUp), and a
// focused terminal takes Tab, Ctrl+C and the arrows rather than letting the
// page have them.

const path = require('path');
const fs = require('fs');

const CHILD = path.join(path.dirname(process.execPath),
                        process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');
const SDLK = { RETURN: 13, TAB: 9, BACKSPACE: 8, UP: 0x40000052, a: 97, c: 99, x: 120 };
const KMOD_LCTRL = 0x0040;

function waitFor(pred, what, ms) {
    const end = Date.now() + (ms || 15000);
    while (Date.now() < end) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(5);
    }
    assert(pred(), 'timed out waiting for ' + what);
    return false;
}

function typeKey(keycode, text, mod) {
    keyDown(keycode, 0, mod || 0);
    if (text) textInput(text);
    keyUp(keycode, 0, mod || 0);
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not built beside bro-headless (BRO_BUILD_TESTS)');
} else {
    const before = document.createElement('input');
    document.body.appendChild(before);
    const t = document.createElement('terminal');
    document.body.appendChild(t);
    flush();

    // a, Enter, Tab, Ctrl+C, Backspace, Up, then IME-committed text.
    const expected = '61' + '0d' + '09' + '03' + '7f' + '1b5b41' + 'c3a9';
    const pid = t.spawn({ command: CHILD, args: ['keys', String(expected.length / 2)] });
    assert(pid > 0 && t.running, 'spawned pty_child, pid ' + pid);
    waitFor(() => t.screenText().includes('READY'), 'READY');

    t.focus();
    advanceTime(16);
    assert(document.activeElement === t, 'focus() focuses the terminal');

    // The page sees keydown first and may keep a key: this one never reaches
    // the child.
    let seen = 0;
    const keep = (e) => { ++seen; if (e.key === 'x') e.preventDefault(); };
    t.addEventListener('keydown', keep);
    typeKey(SDLK.x, 'x');

    typeKey(SDLK.a, 'a');
    typeKey(SDLK.RETURN);
    typeKey(SDLK.TAB);
    assert(document.activeElement === t, 'Tab went to the child, focus stayed');
    typeKey(SDLK.c, null, KMOD_LCTRL);
    typeKey(SDLK.BACKSPACE);
    typeKey(SDLK.UP);
    textInput('é');  // committed text with no key behind it

    waitFor(() => !t.running, 'the child to report and exit');
    const screen = t.screenText();
    const m = /KEYS:([0-9a-f]*)/.exec(screen);
    assert(m, 'the child reported its input: ' + JSON.stringify(screen));
    if (m) assert(m[1] === expected, 'bytes received ' + m[1] + ', want ' + expected);
    assert(seen >= 7, 'the page saw every keydown (' + seen + ')');
    assert(t.exitCode === 0, 'exit code 0, got ' + t.exitCode);
    t.removeEventListener('keydown', keep);

    // The kitty keyboard protocol, once the program pushes it (CSI > 1 u,
    // fed here as the program would print it): Escape and Ctrl+C become
    // unambiguous CSI u sequences, plain text stays text.
    const k = document.createElement('terminal');
    document.body.appendChild(k);
    flush();
    const kittyWant = '1b5b323775' + '1b5b39393b3575' + '62';
    k.spawn({ command: CHILD, args: ['keys', String(kittyWant.length / 2)] });
    waitFor(() => k.screenText().includes('READY'), 'READY (kitty)');
    k.feed('\x1b[>1u');
    k.focus();
    advanceTime(16);
    typeKey(27);                       // Escape: CSI 27 u
    typeKey(SDLK.c, null, KMOD_LCTRL); // Ctrl+C: CSI 99 ; 5 u
    typeKey(98, 'b');                  // b
    waitFor(() => !k.running, 'the kitty child to report');
    const km = /KEYS:([0-9a-f]*)/.exec(k.screenText());
    assert(km && km[1] === kittyWant, 'kitty bytes ' + (km && km[1]) + ', want ' + kittyWant);

    // Keys typed into a terminal that is not focused go elsewhere.
    before.focus();
    advanceTime(16);
    typeKey(SDLK.a, 'a');
    assert(before.value === 'a', 'an unfocused terminal leaves typing to the focused input');
}
