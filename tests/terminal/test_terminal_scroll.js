// <terminal>: the scrollback view.
//
// The wheel scrolls through history when the program is not capturing the
// mouse (three lines a notch by default, `wheelLines`); script scrolls by
// lines, pages, to the top, the bottom, a row. Scrolled up, the view stays
// on its lines while output arrives; typing or pasting returns it to the
// bottom unless scrollOnInput is off. `viewport` and the `scroll` event give
// what a scrollbar needs.

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

function firstLine(t) { return t.frameText().split('\n')[0]; }

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '30');
    t.setAttribute('rows', '5');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    const events = [];
    t.addEventListener('scroll', (e) => events.push(e.detail));

    let text = '';
    for (let i = 0; i < 40; ++i) text += 'line ' + i + '\r\n';
    t.feed(text + 'prompt');
    waitFor(() => t.frameText().includes('prompt'), 'the output is shown');
    const v0 = t.viewport;
    assert(v0.atBottom && v0.rows === 5 && v0.historyRows === 36 && v0.topRow === v0.screenTopRow,
           'at the bottom with 36 rows of history: ' + JSON.stringify(v0));
    assert(events.length > 0 && events[events.length - 1].historyRows === 36 &&
           events[events.length - 1].screenTopRow === v0.screenTopRow,
           'scroll events carry the view: ' + JSON.stringify(events[events.length - 1]));

    // The wheel: three lines a notch, up into history.
    const r = t.getBoundingClientRect();
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    wheel(cx, cy, -1);
    waitFor(() => !t.viewport.atBottom && firstLine(t) === 'line 33', 'one notch up');
    assert(t.viewport.topRow === v0.topRow - 3, 'a notch is three lines: ' + JSON.stringify(t.viewport));
    const last = events[events.length - 1];
    assert(last && !last.atBottom && last.topRow === v0.topRow - 3, 'scroll event: ' + JSON.stringify(last));
    t.options = { wheelLines: 1 };
    wheel(cx, cy, -2);
    waitFor(() => firstLine(t) === 'line 31', 'two notches of one line');
    wheel(cx, cy, 2);
    waitFor(() => firstLine(t) === 'line 33', 'and back down');

    // Output while scrolled up: the view stays on its lines.
    t.feed('\r\nnew output 1\r\nnew output 2\r\n');
    waitFor(() => t.viewport.historyRows === 39, 'history grew');
    advanceTime(16);
    assert(firstLine(t) === 'line 33' && !t.viewport.atBottom, 'the view stays put: ' + firstLine(t));

    // Script scrolling.
    assert(t.scrollLines(-2) === true, 'scrollLines(-2) moves');
    waitFor(() => firstLine(t) === 'line 31', 'two lines up');
    t.scrollPages(-1);
    waitFor(() => firstLine(t) === 'line 27', 'a page up is rows - 1 lines');
    t.scrollToTop();
    waitFor(() => firstLine(t) === 'line 0', 'to the top');
    assert(t.viewport.topRow === t.viewport.firstRow, 'topRow is firstRow at the top');
    assert(t.scrollLines(-1) === false, 'nothing above the top');
    t.scrollToRow(t.viewport.firstRow + 10);
    waitFor(() => firstLine(t) === 'line 10', 'to a row');
    t.scrollToBottom();
    waitFor(() => t.viewport.atBottom && t.frameText().includes('new output 2'), 'to the bottom');

    // Typing returns the view to the bottom; with scrollOnInput off it does not.
    if (fs.existsSync(CHILD)) {
        const k = document.createElement('terminal');
        k.setAttribute('cols', '30');
        k.setAttribute('rows', '3');
        document.body.appendChild(k);
        flush();
        k.spawn({ command: CHILD, args: ['keys', '2'] });
        waitFor(() => k.screenText().includes('READY'), 'READY');
        let lots = '';
        for (let i = 0; i < 20; ++i) lots += 'x ' + i + '\r\n';
        k.feed(lots);
        waitFor(() => k.viewport.historyRows >= 18, 'history');
        k.options = { scrollOnInput: false };
        k.scrollToTop();
        waitFor(() => !k.viewport.atBottom, 'scrolled up');
        k.paste('a');
        advanceTime(16);
        assert(!k.viewport.atBottom, 'scrollOnInput off: input leaves the view where it is');
        k.options = { scrollOnInput: true };
        k.paste('b');
        waitFor(() => k.viewport.atBottom, 'input snaps the view to the bottom');
        waitFor(() => k.screenText().includes('KEYS:6162'), 'both reached the child');
    }
}
