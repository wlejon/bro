// <terminal> at a 2x render scale. CSS px stay window coordinates, so the
// grid keeps its size in CSS px; the cell is snapped to whole device pixels
// at the scale (crisp edges), the PTY is told its size in device pixels,
// and SGR-pixel mouse reports count device pixels.

const path = require('path');
const fs = require('fs');

const CHILD = path.join(path.dirname(process.execPath),
                        process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');

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

function whole(v) { return Math.abs(v - Math.round(v)) < 1e-3; }

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '100');
    t.setAttribute('rows', '4');
    t.style.cssText = 'font: 13px monospace; letter-spacing: 0.3px; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    const m1 = t.metrics;
    assert(m1.scale === 1 && whole(m1.cellWidth) && whole(m1.cellHeight),
           'at 1x the cell is whole CSS px: ' + JSON.stringify(m1));

    setDeviceScaleFactor(2);
    flush();
    advanceTime(16);
    advanceTime(16);
    const m2 = t.metrics;
    assert(m2.scale === 2, 'the metrics follow the render scale: ' + JSON.stringify(m2));
    assert(whole(m2.cellWidth * 2) && whole(m2.cellHeight * 2) && whole(m2.baseline * 2),
           'the cell and baseline snap to device px: ' + JSON.stringify(m2));
    assert(m2.pixelWidth === Math.round(m2.cellWidth * 2) && m2.pixelHeight === Math.round(m2.cellHeight * 2),
           'the cell in device px: ' + JSON.stringify(m2));
    assert(Math.abs(m2.cellWidth - m1.cellWidth) <= 0.5 && Math.abs(m2.cellHeight - m1.cellHeight) <= 0.5,
           'the grid keeps its CSS size (within a half px of snapping)');

    if (fs.existsSync(CHILD)) {
        // SGR-pixel reports count device pixels.
        t.spawn({ command: CHILD, args: ['keys', '20'] });
        waitFor(() => t.screenText().includes('READY'), 'READY');
        t.feed('\x1b[?1000h\x1b[?1016h');
        const r = t.getBoundingClientRect();
        const x = r.left + 3.5 * m2.cellWidth, y = r.top + 1.5 * m2.cellHeight;
        mouseDown(x, y, 0);
        mouseUp(x, y, 0);
        const px = Math.floor((x - r.left) * 2) + 1, py = Math.floor((y - r.top) * 2) + 1;
        const want = '\x1b[<0;' + px + ';' + py + 'M\x1b[<0;' + px + ';' + py + 'm';
        const hex = Array.from(want, (c) => c.charCodeAt(0).toString(16).padStart(2, '0')).join('');
        // keys reads exactly 20 bytes; the report is shorter or longer by
        // its digits, so compare the prefix it read.
        waitFor(() => t.screenText().includes('KEYS:'), 'the child read the report');
        const got = t.screenText().split('\n').find((l) => l.startsWith('KEYS:')).slice(5);
        assert(hex.startsWith(got) || got.startsWith(hex), 'SGR-pixels at 2x: got ' + got + ', want ' + hex);
    }
    if (fs.existsSync(CHILD) && process.platform !== 'win32') {
        // The PTY's pixel size (TIOCGWINSZ) is the grid in device pixels.
        const p = document.createElement('terminal');
        p.setAttribute('cols', '20');
        p.setAttribute('rows', '3');
        p.style.cssText = 'font: 13px monospace; padding: 0; border: 0; display: block';
        document.body.appendChild(p);
        flush();
        advanceTime(16);
        p.spawn({ command: CHILD, args: ['pixels'] });
        waitFor(() => p.screenText().includes('PIXELS'), 'PIXELS');
        const m = p.metrics;
        const want = 'PIXELS ' + (20 * m.pixelWidth) + 'x' + (3 * m.pixelHeight);
        assert(p.screenText().includes(want), 'winsize pixels: ' + p.screenText() + ', want ' + want);
        p.write('q');
    }
    setDeviceScaleFactor(1);
    flush();
}
