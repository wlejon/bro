// <terminal> is its own compositor layer: a change to the page does not
// re-record the terminal's paint, and the terminal's output does not
// invalidate the page's cached paint. (Headless records at each capture;
// tests/windowed/terminal_layer checks the windowed frame loop's counters.)

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

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const note = document.createElement('div');
    note.textContent = 'page text 0';
    document.body.appendChild(note);
    const t = document.createElement('terminal');
    t.setAttribute('cols', '20');
    t.setAttribute('rows', '3');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    t.feed('ready');
    waitFor(() => t.frameText().includes('ready'), 'output shown');
    const r = t.getBoundingClientRect();
    const cell = (col, row) => [r.left + (col + 0.5) * t.metrics.cellWidth, r.top + (row + 0.5) * t.metrics.cellHeight];
    getPixel(...cell(0, 0));  // a capture records the layer
    const recorded = t.layerRecords;
    assert(recorded > 0, 'the terminal was recorded into its layer: ' + recorded);

    // Page changes: the terminal is not re-recorded.
    for (let i = 1; i <= 5; ++i) {
        note.textContent = 'page text ' + i;
        note.style.color = i % 2 ? 'red' : 'blue';
        flush();
        advanceTime(16);
        getPixel(...cell(0, 0));
    }
    assert(t.layerRecords === recorded, 'page changes leave the terminal\'s layer alone: ' +
           recorded + ' -> ' + t.layerRecords);

    // Terminal output: re-recorded, and the page is not invalidated.
    const before = bro.terminal.stats();
    t.feed('\r\n\x1b[41m    \x1b[0mz');
    waitFor(() => t.frameText().includes('z'), 'the new row');
    for (let i = 0; i < 5; ++i) advanceTime(16);
    const after = bro.terminal.stats();
    assert(after.pageInvalidations === before.pageInvalidations,
           'terminal output does not invalidate the page: ' + before.pageInvalidations + ' -> ' + after.pageInvalidations);
    const red = getPixel(...cell(1, 1));
    assert(red.r > 150 && red.g < 80, 'the new output is composited: ' + JSON.stringify(red));
    assert(t.layerRecords === recorded + 1, 'one new recording for it: ' + recorded + ' -> ' + t.layerRecords);
    assert(bro.terminal.stats().layerRecords >= after.layerRecords + 1, 'bro.terminal.stats() counts it');

    // An idle terminal is not re-recorded by further captures.
    getPixel(...cell(1, 1));
    getPixel(...cell(1, 1));
    assert(t.layerRecords === recorded + 1, 'idle: no recordings');

    // In a scrolled container the layer moves and clips with the page.
    const box = document.createElement('div');
    box.style.cssText = 'height: 40px; overflow: hidden';
    document.body.appendChild(box);
    const inner = document.createElement('terminal');
    inner.setAttribute('cols', '10');
    inner.setAttribute('rows', '4');
    inner.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    box.appendChild(inner);
    flush();
    inner.feed('\x1b[44m         a\r\n         b\r\n         c\r\n         d\x1b[0m');
    waitFor(() => inner.frameText().includes('d'), 'inner output');
    advanceTime(16);
    const br = box.getBoundingClientRect();
    const inside = getPixel(br.left + 5, br.top + 20);
    const below = getPixel(br.left + 5, br.top + 50);
    assert(inside.b > 150 && inside.r < 80, 'the layer shows inside the clip: ' + JSON.stringify(inside));
    assert(!(below.b > 150 && below.r < 80), 'and is clipped below it: ' + JSON.stringify(below));
}
