// <terminal>: links. OSC 8 hyperlinks and detected URLs and paths are found
// under a cell (linkAt), underlined while the pointer is over them (the
// pointer becomes a hand), and Ctrl+click (Cmd+click on macOS) raises a
// cancellable `linkactivate`; its default opens the link through the OS,
// which headless never does.

const SDLK = { LCTRL: 0x400000E0, LGUI: 0x400000E3 };
const MAC = process.platform === 'darwin';

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

function cellPoint(t, col, row) {
    const r = t.getBoundingClientRect();
    const m = t.metrics;
    return [r.left + (col + 0.5) * m.cellWidth, r.top + (row + 0.5) * m.cellHeight];
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '60');
    t.setAttribute('rows', '4');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);

    t.feed('see \x1b]8;id=x;https://example.com/docs\x1b\\the docs\x1b]8;;\x1b\\ now\r\n' +
           'or visit https://bro.dev/a?b=1 today\r\n' +
           'edit /tmp/notes.txt please');
    waitFor(() => t.frameText().includes('please'), 'output shown');
    const top = t.viewport.topRow;

    const h = t.linkAt(top, 6);
    assert(h && h.kind === 'hyperlink' && h.uri === 'https://example.com/docs' && h.text === 'the docs',
           'OSC 8 hyperlink: ' + JSON.stringify(h));
    assert(h.range.startCol === 4 && h.range.endCol === 12, 'its range: ' + JSON.stringify(h.range));
    const u = t.linkAt(top + 1, 12);
    assert(u && u.kind === 'url' && u.uri === 'https://bro.dev/a?b=1', 'detected URL: ' + JSON.stringify(u));
    const p = t.linkAt(top + 2, 8);
    assert(p && p.kind === 'path' && p.uri === '/tmp/notes.txt', 'detected path: ' + JSON.stringify(p));
    assert(t.linkAt(top, 0) === null, 'no link on plain text');

    // Hover: the hand, and back to the I-beam off the link.
    mouseMove(...cellPoint(t, 6, 0));
    assert(currentCursor() === 'pointer', 'the hand over a link: ' + currentCursor());
    mouseMove(...cellPoint(t, 1, 0));
    assert(currentCursor() === 'text', 'the I-beam off it: ' + currentCursor());

    // Ctrl/Cmd+click: linkactivate, cancellable.
    const seen = [];
    t.addEventListener('linkactivate', (e) => { seen.push(e.detail); e.preventDefault(); });
    const chordKey = MAC ? SDLK.LGUI : SDLK.LCTRL;
    const chordMod = MAC ? 0x0400 : 0x0040;
    keyDown(chordKey, 0, chordMod);
    click(...cellPoint(t, 15, 1));
    keyUp(chordKey, 0, 0);
    assert(seen.length === 1 && seen[0].uri === 'https://bro.dev/a?b=1' && seen[0].kind === 'url' && seen[0].text,
           'linkactivate: ' + JSON.stringify(seen));
    assert(t.selection === null, 'a link click selects nothing');

    // A plain click on a link is a click: no event.
    click(...cellPoint(t, 6, 0));
    assert(seen.length === 1, 'without the modifier, no linkactivate');
}
