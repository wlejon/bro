// <terminal> inline images, end to end through the compositor: a kitty
// placement, a sixel image and an animated iTerm2 GIF land on exactly the
// cells bropty placed them in (read back from the screen, at 1x and 2x),
// scroll with the text, animate on their own, and go when the quota is
// lowered. bro_terminal_test checks the same placement cell by cell against
// bropty's image state, for real programs' output too.

function b64(bytes) {
    const tab = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let out = '';
    let i = 0;
    for (; i + 2 < bytes.length; i += 3) {
        const v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += tab[(v >> 18) & 63] + tab[(v >> 12) & 63] + tab[(v >> 6) & 63] + tab[v & 63];
    }
    if (i < bytes.length) {
        const v = (bytes[i] << 16) | ((i + 1 < bytes.length ? bytes[i + 1] : 0) << 8);
        out += tab[(v >> 18) & 63] + tab[(v >> 12) & 63] + (i + 1 < bytes.length ? tab[(v >> 6) & 63] : '=') + '=';
    }
    return out;
}

function solid(w, h, rgba) {
    const px = [];
    for (let i = 0; i < w * h; ++i) px.push(rgba[0], rgba[1], rgba[2], rgba[3]);
    return px;
}

const kitty = (control, bytes) => '\x1b_G' + control + (bytes ? ';' + b64(bytes) : '') + '\x1b\\';

// A 16 x 12 GIF, three solid frames (red, green, blue) of 70 ms each.
const ANIM_GIF = [
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x0c, 0x00, 0xf0, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00,
    0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0c,
    0x00, 0x00, 0x02, 0x0c, 0x84, 0x8f, 0xa9, 0xcb, 0xed, 0x0f, 0xa3, 0x9c, 0xb4, 0xda, 0x6b, 0x0a, 0x00, 0x21,
    0xf9, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0c, 0x00, 0x80, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0c, 0x84, 0x8f, 0xa9, 0xcb, 0xed, 0x0f, 0xa3, 0x9c, 0xb4, 0xda, 0x6b,
    0x0a, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0c,
    0x00, 0x80, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x02, 0x0c, 0x84, 0x8f, 0xa9, 0xcb, 0xed, 0x0f, 0xa3, 0x9c,
    0xb4, 0xda, 0x6b, 0x0a, 0x00, 0x3b];

function frames(n) { for (let i = 0; i < n; ++i) advanceTime(16); }

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '40');
    t.setAttribute('rows', '10');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block; position: absolute; ' +
                      'left: 0; top: 0; --terminal-background: #000000';
    document.body.style.margin = '0';
    document.body.appendChild(t);
    flush();
    frames(2);
    t.feed('\x1b[?25l');  // no cursor over the cells being probed

    // The colour at cell (col, row) + (fx, fy) of a cell, read off the screen.
    function at(col, row, fx, fy) {
        const m = t.metrics, r = t.getBoundingClientRect();
        const x = r.left + (col + fx) * m.cellWidth, y = r.top + (row + fy) * m.cellHeight;
        const p = getPixels(Math.floor(x), Math.floor(y), 1, 1).data;
        return [p[0], p[1], p[2]];
    }
    const isColour = (c, want) => Math.abs(c[0] - want[0]) <= 8 && Math.abs(c[1] - want[1]) <= 8 &&
                                  Math.abs(c[2] - want[2]) <= 8;
    const RED = [255, 0, 0], BLACK = [0, 0, 0], BLUE = [0, 0, 255];

    // The image's cells are red edge to edge, their neighbours are not: the
    // box is exact to the cell, probed just inside and just outside each edge.
    function checkBox(col, row, cols, rows, colour, what) {
        const e = 0.6 / Math.max(1, t.metrics.scale) / t.metrics.cellWidth;  // ~half a device px
        const ey = 0.6 / Math.max(1, t.metrics.scale) / t.metrics.cellHeight;
        const inside = [[col, row, 0.5, 0.5], [col, row, e * 2, ey * 2], [col + cols - 1, row + rows - 1, 1 - e * 2, 1 - ey * 2],
                        [col + cols - 1, row, 0.5, 0.5], [col, row + rows - 1, 0.5, 0.5]];
        for (const [c, r, fx, fy] of inside) {
            const got = at(c, r, fx, fy);
            assert(isColour(got, colour), what + ': inside at ' + [c, r, fx.toFixed(3), fy.toFixed(3)] + ' got ' + got);
        }
        const outside = [[col - 1, row, 1 - e * 2, 0.5], [col + cols, row, e * 2, 0.5],
                         [col, row - 1, 0.5, 1 - ey * 2], [col, row + rows, 0.5, ey * 2]];
        for (const [c, r, fx, fy] of outside) {
            if (r < 0 || c < 0) continue;
            const got = at(c, r, fx, fy);
            assert(isColour(got, BLACK), what + ': outside at ' + [c, r, fx.toFixed(3), fy.toFixed(3)] + ' got ' + got);
        }
    }

    // ── kitty: a 4 x 4 red image stretched over 3 x 2 cells at (5, 2) ──
    t.feed('\x1b[3;6H' + kitty('a=T,f=32,s=4,v=4,i=1,c=3,r=2,C=1,q=2', solid(4, 4, [255, 0, 0, 255])));
    frames(2);
    assert(t.images.count === 1 && t.images.placements === 1 && t.images.bytes === 64,
           'one image held: ' + JSON.stringify(t.images));
    assert(t.images.limit === 320 * 1024 * 1024, 'the default quota: ' + t.images.limit);
    checkBox(5, 2, 3, 2, RED, 'kitty at 1x');

    // At a 2x render scale the cells re-snap and the image follows them.
    setDeviceScaleFactor(2);
    flush();
    frames(3);
    assert(t.metrics.scale === 2, 'at 2x');
    checkBox(5, 2, 3, 2, RED, 'kitty at 2x');

    // Output scrolls it with its text: three lines at the bottom move it up three rows.
    t.feed('\x1b[10;1H\n\n\n');
    frames(2);
    // Its top row is now in history (row -1); its bottom row is screen row 0.
    const top = at(5, 0, 0.5, 0.5);
    assert(isColour(top, RED), 'scrolled with the text: row 0 shows its lower half, got ' + top);
    assert(isColour(at(5, 1, 0.5, 0.5), BLACK), 'and row 1 no longer');
    // Scrolled back, the whole image is there again.
    t.scrollLines(-3);
    frames(2);
    checkBox(5, 2, 3, 2, RED, 'scrolled back');
    t.scrollToBottom();
    frames(2);

    // ── sixel: 12 x 24 blue at (20, 5), on the text plane ──
    let six = '\x1bPq#1;2;0;0;100#1';
    for (let band = 0; band < 4; ++band) six += '!12~' + (band < 3 ? '-' : '');
    six += '\x1b\\';
    t.feed('\x1b[6;21H' + six);
    frames(2);
    assert(t.images.count === 2, 'the sixel image is held: ' + JSON.stringify(t.images));
    // 12 x 24 device-independent image pixels at the terminal's cell size:
    // its first cell is fully covered.
    const m = t.metrics;
    const sw = 12 / m.pixelWidth, sh = 24 / m.pixelHeight;  // its extent in cells
    assert(isColour(at(20, 5, Math.min(0.5, sw / 2), Math.min(0.5, sh / 2)), BLUE), 'sixel drawn at its cell');
    // Text written over it replaces that part of it (image cells are text).
    t.feed('\x1b[6;21HZ');
    frames(2);
    assert(!isColour(at(20, 5, 0.5, 0.5), BLUE), 'text over a sixel cell replaces it');

    // ── iTerm2: an animated GIF runs by itself (no output, no input) ──
    t.feed('\x1b[2;30H\x1b]1337;File=inline=1;width=2;height=2;preserveAspectRatio=0;size=' + ANIM_GIF.length + ':' +
           b64(ANIM_GIF) + '\x07');
    const seen = new Set();
    const end = Date.now() + 3000;
    while (Date.now() < end && seen.size < 3) {
        advanceTime(16);
        const c = at(29, 1, 0.5, 0.5);
        if (isColour(c, [255, 0, 0])) seen.add('r');
        if (isColour(c, [0, 255, 0])) seen.add('g');
        if (isColour(c, [0, 0, 255])) seen.add('b');
        wallSleep(10);
    }
    assert(seen.size === 3, 'the GIF animated through its frames: saw ' + [...seen].join(','));

    // ── the quota: lowered to nothing, every image goes from the screen ──
    t.options = { imageMemoryLimit: 0 };
    assert(t.options.imageMemoryLimit === 0, 'the option reads back');
    frames(3);
    assert(t.images.count === 0 && t.images.bytes === 0 && t.images.limit === 0,
           'nothing held: ' + JSON.stringify(t.images));
    assert(isColour(at(5, 0, 0.5, 0.5), BLACK) && isColour(at(29, 1, 0.5, 0.5), BLACK), 'and nothing drawn');
    let threw = false;
    try { t.options = { imageMemoryLimit: -1 }; } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'a negative quota is a TypeError');
    t.options = { imageMemoryLimit: 64 * 1024 * 1024 };
    t.feed(kitty('a=T,f=32,s=4,v=4,i=2,c=1,r=1,C=1,q=2', solid(4, 4, [255, 0, 0, 255])));
    frames(2);
    assert(t.images.count === 1 && t.images.limit === 64 * 1024 * 1024, 'and back: ' + JSON.stringify(t.images));
}
