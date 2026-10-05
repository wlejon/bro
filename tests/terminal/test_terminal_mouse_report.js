// <terminal>: mouse reports, byte for byte, as the program receives them.
//
// bropty's pty_child (`keys N`) prints the hex of the next N bytes it reads.
// The program's mouse modes are set by feeding the sequences it would write
// (feed() goes through the same parser). Each mode and encoding: X10 (?9),
// normal (?1000) in the default and SGR (?1006) encodings, button-event
// (?1002) and any-event (?1003) motion, SGR-pixels (?1016), the wheel, and
// alternate scroll (?1007) on the alternate screen. Shift bypasses reporting
// and selects instead.

const path = require('path');
const fs = require('fs');

const CHILD = path.join(path.dirname(process.execPath),
                        process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');
const SDLK_LSHIFT = 0x400000E1;

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

function hexOf(s) {
    let h = '';
    for (let i = 0; i < s.length; ++i) h += s.charCodeAt(i).toString(16).padStart(2, '0');
    return h;
}

function cellPoint(t, col, row) {
    const r = t.getBoundingClientRect();
    const m = t.metrics;
    return [r.left + (col + 0.25) * m.cellWidth, r.top + (row + 0.5) * m.cellHeight];
}

// A fresh terminal running `keys <bytes>` with `modes` fed in; `act` drives
// the pointer; the child must then report exactly `expected`.
function scenario(name, modes, expected, act) {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '100');  // the KEYS: line on one row
    t.setAttribute('rows', '4');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    const want = typeof expected === 'function' ? expected(t) : expected;
    t.spawn({ command: CHILD, args: ['keys', String(want.length)] });
    waitFor(() => t.screenText().includes('READY'), name + ': READY');
    t.feed(modes);
    act(t);
    waitFor(() => t.screenText().includes('KEYS:'), name + ': the child read the report');
    const line = t.screenText().split('\n').find((l) => l.startsWith('KEYS:')) || '';
    assert(line === 'KEYS:' + hexOf(want), name + ': got ' + line + ', want KEYS:' + hexOf(want));
    t.remove();
    flush();
    return t;
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not built beside bro-headless (BRO_BUILD_TESTS)');
} else {
    const press = (t, col, row, button) => mouseDown(...cellPoint(t, col, row), button || 0);
    const release = (t, col, row, button) => mouseUp(...cellPoint(t, col, row), button || 0);
    const clickAt = (t, col, row) => { press(t, col, row); release(t, col, row); };
    const C = (n) => String.fromCharCode(32 + n);

    // X10: presses only, the default encoding (Cb Cx Cy offset by 32, 1-based).
    scenario('X10', '\x1b[?9h', '\x1b[M' + C(0) + C(4) + C(2), (t) => clickAt(t, 3, 1));

    // Normal tracking, default encoding: the release is button 3.
    scenario('normal', '\x1b[?1000h',
             '\x1b[M' + C(0) + C(4) + C(2) + '\x1b[M' + C(3) + C(4) + C(2), (t) => clickAt(t, 3, 1));

    // SGR: the button on release too, M / m.
    scenario('SGR', '\x1b[?1000h\x1b[?1006h', '\x1b[<0;4;2M\x1b[<0;4;2m', (t) => clickAt(t, 3, 1));

    // Right button.
    scenario('SGR right', '\x1b[?1000h\x1b[?1006h', '\x1b[<2;2;1M\x1b[<2;2;1m', (t) => {
        press(t, 1, 0, 2);
        release(t, 1, 0, 2);
    });

    // Button-event tracking: motion while a button is held (+32).
    scenario('button-event', '\x1b[?1002h\x1b[?1006h', '\x1b[<0;4;2M\x1b[<32;6;2M\x1b[<0;6;2m', (t) => {
        press(t, 3, 1);
        mouseMove(...cellPoint(t, 5, 1));
        release(t, 5, 1);
    });

    // Any-event tracking: motion with no button (35 = 32 + 3).
    scenario('any-event', '\x1b[?1003h\x1b[?1006h', '\x1b[<35;3;1M', (t) => {
        mouseMove(...cellPoint(t, 2, 0));
        assert(currentCursor() === 'default', 'the arrow while the program has the mouse: ' + currentCursor());
    });

    // SGR-pixels: device pixels from the grid's origin, 1-based.
    scenario('SGR-pixels', '\x1b[?1000h\x1b[?1016h', (t) => {
        const r = t.getBoundingClientRect();
        const [x, y] = cellPoint(t, 3, 1);
        const s = t.metrics.scale;
        const px = Math.floor((x - r.left) * s) + 1, py = Math.floor((y - r.top) * s) + 1;
        return '\x1b[<0;' + px + ';' + py + 'M\x1b[<0;' + px + ';' + py + 'm';
    }, (t) => clickAt(t, 3, 1));

    // The wheel, reported (64 up, 65 down).
    scenario('wheel', '\x1b[?1000h\x1b[?1006h', '\x1b[<64;4;2M\x1b[<65;4;2M', (t) => {
        wheel(...cellPoint(t, 3, 1), -1);
        wheel(...cellPoint(t, 3, 1), 1);
    });

    // Shift bypasses reporting: the drag selects and nothing is sent; the
    // plain click after it is reported.
    scenario('shift bypass', '\x1b[?1000h\x1b[?1006h', '\x1b[<0;2;1M\x1b[<0;2;1m', (t) => {
        keyDown(SDLK_LSHIFT, 0, 0x0001);
        press(t, 0, 0);
        mouseMove(...cellPoint(t, 4, 0));
        release(t, 4, 0);
        keyUp(SDLK_LSHIFT, 0, 0);
        assert(t.selection !== null, 'Shift+drag selects while the program reports');
        clickAt(t, 1, 0);
    });

    // Alternate scroll: on the alternate screen with no reporting, the wheel
    // sends cursor keys (DECCKM's SS3 form when that is set).
    scenario('alternate scroll', '\x1b[?1049h\x1b[?1007h', '\x1b[A\x1b[B', (t) => {
        wheel(...cellPoint(t, 3, 1), -1);
        wheel(...cellPoint(t, 3, 1), 1);
    });
    scenario('alternate scroll DECCKM', '\x1b[?1049h\x1b[?1007h\x1b[?1h', '\x1bOA', (t) => {
        wheel(...cellPoint(t, 3, 1), -1);
    });
}
