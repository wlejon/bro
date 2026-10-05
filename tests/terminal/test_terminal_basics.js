// <terminal>: the element, its namespace, its size and what it paints.
//
// The element is a replaced box sized in cells of its own font (cols x rows
// attributes, default 80 x 24); the grid then follows the box. Output fed
// into it reaches the screen text, the presented frame and the pixels, and
// history past the screen goes to the scrollback.

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    assert(typeof bro.terminal.defaultShell === 'string' && bro.terminal.defaultShell.length > 0,
           'bro.terminal.defaultShell names a shell: ' + bro.terminal.defaultShell);

    const t = document.createElement('terminal');
    assert(t instanceof HTMLTerminalElement, 'a <terminal> is an HTMLTerminalElement');
    assert(t instanceof HTMLElement, '... and an HTMLElement');
    assert(t.tabIndex === 0, 'it is focusable like a form control (tabIndex ' + t.tabIndex + ')');
    document.body.appendChild(t);
    flush();
    advanceTime(16);

    // Default grid, in cells of the UA monospace font.
    assert(t.cols === 80 && t.rows === 24, 'default grid 80x24, got ' + t.cols + 'x' + t.rows);
    const r = t.getBoundingClientRect();
    assert(r.width > 200 && r.height > 100, 'the box has a size: ' + r.width + 'x' + r.height);
    assert(getComputedStyle(t).display === 'block', 'display: block');

    // The cols/rows attributes size the box.
    const small = document.createElement('terminal');
    small.setAttribute('cols', '20');
    small.setAttribute('rows', '5');
    document.body.appendChild(small);
    flush();
    advanceTime(16);
    assert(small.cols === 20 && small.rows === 5, 'cols/rows attributes: ' + small.cols + 'x' + small.rows);
    const sr = small.getBoundingClientRect();
    assert(Math.abs(sr.width / r.width - 20 / 80) < 0.02, 'width scales with cols');

    // Output reaches the screen, the presented frame, the cursor.
    t.feed('hello \x1b[1mworld\x1b[0m\r\nline two');
    let shown = false;
    for (let i = 0; i < 100 && !shown; ++i) {
        advanceTime(16);
        shown = t.frameText().includes('line two');
        if (!shown) wallSleep(2);
    }
    assert(t.screenText() === 'hello world\nline two', 'screenText: ' + JSON.stringify(t.screenText()));
    assert(shown, 'the presented frame shows the output: ' + JSON.stringify(t.frameText()));
    const c = t.cursor;
    assert(c.row === 1 && c.col === 8 && c.visible && c.shape === 'block', 'cursor ' + JSON.stringify(c));
    t.feed('\x1b[6 q\x1b]0;my title\x07');
    assert(t.cursor.shape === 'bar', 'DECSCUSR shape reaches the cursor');
    assert(t.title === 'my title', 'OSC 0 title: ' + t.title);

    // History past the screen.
    small.feed('\x1b[2J\x1b[H');
    for (let i = 0; i < 12; ++i) small.feed('row ' + i + (i < 11 ? '\r\n' : ''));
    assert(small.screenText() === 'row 7\nrow 8\nrow 9\nrow 10\nrow 11',
           'screen keeps the last rows: ' + JSON.stringify(small.screenText()));
    assert(small.scrollbackText().endsWith('row 5\nrow 6'),
           'scrollback has the rest: ' + JSON.stringify(small.scrollbackText()));

    // Pixels: a red background block at the top-left of the content box,
    // the default background elsewhere, and glyph ink in the text.
    const p = document.createElement('terminal');
    p.setAttribute('cols', '10');
    p.setAttribute('rows', '3');
    p.style.font = '16px monospace';
    document.body.appendChild(p);
    flush();
    p.feed('\x1b[41m    \x1b[0m\r\n\x1b[97mMMMM');
    for (let i = 0; i < 50 && !p.frameText().includes('MMMM'); ++i) { advanceTime(16); wallSleep(2); }
    advanceTime(16);
    const pr = p.getBoundingClientRect();
    const cellW = pr.width / 10, cellH = pr.height / 3;
    const red = getPixel(pr.left + cellW * 1.5, pr.top + cellH * 0.5);
    assert(red.r > 150 && red.g < 80 && red.b < 80, 'SGR 41 paints the cell red: ' + JSON.stringify(red));
    const bg = getPixel(pr.left + cellW * 7.5, pr.top + cellH * 0.5);
    assert(bg.r < 30 && bg.g < 30 && bg.b < 30, 'the default background is dark: ' + JSON.stringify(bg));
    const ink = getPixels(Math.round(pr.left), Math.round(pr.top + cellH), Math.round(cellW * 4), Math.round(cellH));
    let bright = 0;
    for (let i = 0; i < ink.data.length; i += 4) if (ink.data[i] > 180 && ink.data[i + 1] > 180) ++bright;
    assert(bright > 20, 'the glyphs leave ink: ' + bright + ' bright pixels');

    // Not focused: no keys reach it, and nothing was spawned.
    assert(t.running === false && t.pid === 0 && t.exitCode === null, 'no child yet');
}
