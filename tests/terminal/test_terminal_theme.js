// <terminal>: colours. The palette comes from bropty's standard one, then
// CSS (an element with a background-color uses it and its color; the
// --terminal-* custom properties set single slots), then script (`theme`),
// later winning slot by slot. The result is the base palette the program's
// own OSC 4/10/11 changes reset back to. boldIsBright and minimumContrast
// change how cells resolve. Checked by sampling cell pixels (a full block,
// U+2588, shows the foreground over its whole cell).

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

function makeTerminal() {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '10');
    t.setAttribute('rows', '2');
    t.style.cssText = 'font: 20px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

function cellPixel(t, col, row) {
    advanceTime(16);
    const r = t.getBoundingClientRect();
    const m = t.metrics;
    return getPixel(r.left + (col + 0.5) * m.cellWidth, r.top + (row + 0.5) * m.cellHeight);
}

function near(p, rgb, tol) {
    tol = tol || 3;
    return Math.abs(p.r - rgb[0]) <= tol && Math.abs(p.g - rgb[1]) <= tol && Math.abs(p.b - rgb[2]) <= tol;
}

function luminance(p) {
    const c = [p.r, p.g, p.b].map((v) => {
        v /= 255;
        return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
    });
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const BLOCK = '█';
    const t = makeTerminal();
    t.feed('\x1b[41m  \x1b[0m ' + BLOCK);
    waitFor(() => t.frameText().includes(BLOCK), 'output shown');
    assert(near(cellPixel(t, 5, 1), [12, 12, 12]), 'the standard background: ' + JSON.stringify(cellPixel(t, 5, 1)));

    // Script's theme.
    t.theme = { background: '#203040', foreground: '#ffeedd' };
    assert(t.theme.background === '#203040' && t.theme.foreground === '#ffeedd', 'theme reads back resolved');
    assert(t.theme.ansi.length === 256 && /^#[0-9a-f]{6}$/.test(t.theme.ansi[1]), 'all 256 colours in the theme');
    assert(t.theme.ansi[196] === '#ff0000' && t.theme.ansi[232] === '#080808', 'the xterm table past 16: ' +
           t.theme.ansi[196] + ' ' + t.theme.ansi[232]);
    waitFor(() => near(cellPixel(t, 5, 1), [0x20, 0x30, 0x40]), 'the theme background is painted');
    assert(near(cellPixel(t, 3, 0), [0xff, 0xee, 0xdd]), 'the theme foreground: ' + JSON.stringify(cellPixel(t, 3, 0)));

    // CSS custom properties set slots; script wins over them.
    t.style.setProperty('--terminal-color-1', '#00ff00');
    flush();
    waitFor(() => near(cellPixel(t, 0, 0), [0, 255, 0]), 'SGR 41 takes --terminal-color-1');
    t.theme = { ansi: [null, '#0000ff'] };
    waitFor(() => near(cellPixel(t, 0, 0), [0, 0, 255]), 'script beats CSS for color 1');
    t.theme = { ansi: [null, null] };
    waitFor(() => near(cellPixel(t, 0, 0), [0, 255, 0]), 'clearing it returns the slot to CSS');

    // The 256-colour table past the ANSI 16: CSS and script, the same way.
    t.feed('\r\n\x1b[48;5;200m \x1b[48;5;201m \x1b[0m');
    t.style.setProperty('--terminal-color-200', '#123456');
    flush();
    waitFor(() => near(cellPixel(t, 0, 1), [0x12, 0x34, 0x56]), '--terminal-color-200');
    assert(t.theme.ansi[200] === '#123456', 'resolved: ' + t.theme.ansi[200]);
    const table = [];
    table[201] = '#abcdef';
    t.theme = { ansi: table };
    waitFor(() => near(cellPixel(t, 1, 1), [0xab, 0xcd, 0xef]), 'theme.ansi[201]');
    assert(near(cellPixel(t, 0, 1), [0x12, 0x34, 0x56]), 'a hole leaves CSS its slot');
    let tooMany = false;
    try { t.theme = { ansi: new Array(257).fill('#000000') }; } catch (e) { tooMany = e instanceof TypeError; }
    assert(tooMany, 'more than 256 colours throws');

    // The program's own changes reset to the base palette.
    t.feed('\x1b]11;#ff0000\x07');
    waitFor(() => near(cellPixel(t, 5, 1), [255, 0, 0]), 'OSC 11 sets the background');
    t.feed('\x1b]111\x07');
    waitFor(() => near(cellPixel(t, 5, 1), [0x20, 0x30, 0x40]), 'OSC 111 resets it to the theme');

    // An element given a background-color takes it and its color.
    const c = makeTerminal();
    c.style.backgroundColor = 'rgb(250, 248, 240)';
    c.style.color = 'rgb(20, 30, 40)';
    flush();
    c.feed(BLOCK);
    waitFor(() => near(cellPixel(c, 5, 1), [250, 248, 240]), 'background-color is the background');
    assert(near(cellPixel(c, 0, 0), [20, 30, 40]), 'color is the foreground: ' + JSON.stringify(cellPixel(c, 0, 0)));
    c.style.setProperty('--terminal-background', '#102030');
    flush();
    waitFor(() => near(cellPixel(c, 5, 1), [0x10, 0x20, 0x30]), '--terminal-background beats background-color');

    // Bold is bright, when asked.
    const b = makeTerminal();
    b.theme = { ansi: [null, '#800000', null, null, null, null, null, null, null, '#ff4040'] };
    b.feed('\x1b[1;31m' + BLOCK);
    waitFor(() => near(cellPixel(b, 0, 0), [0x80, 0, 0]), 'bold red is color 1 by default');
    b.options = { boldIsBright: true };
    waitFor(() => near(cellPixel(b, 0, 0), [0xff, 0x40, 0x40]), 'boldIsBright: color 9');

    // Minimum contrast: a foreground too close to its background is moved.
    const m = makeTerminal();
    m.theme = { background: '#181818', foreground: '#222222' };
    m.feed(BLOCK);
    waitFor(() => near(cellPixel(m, 0, 0), [0x22, 0x22, 0x22]), 'the low-contrast foreground as set');
    m.options = { minimumContrast: 4.5 };
    waitFor(() => !near(cellPixel(m, 0, 0), [0x22, 0x22, 0x22]), 'minimumContrast changes it');
    const fg = cellPixel(m, 0, 0), bg = cellPixel(m, 5, 1);
    const ratio = (Math.max(luminance(fg), luminance(bg)) + 0.05) / (Math.min(luminance(fg), luminance(bg)) + 0.05);
    assert(ratio >= 4.4, 'contrast ratio ' + ratio.toFixed(2) + ' >= 4.5');
    let threw = false;
    try { m.options = { minimumContrast: 30 }; } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'minimumContrast outside 1..21 throws');
}
