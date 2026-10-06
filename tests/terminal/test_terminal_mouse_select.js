// <terminal>: selecting with the pointer.
//
// A drag selects by character, a double click by word, a triple click by
// line, Alt+drag a block; Shift+click extends the selection and a click that
// does not move clears it. With copyOnSelect the selection goes to the
// clipboard, and a middle click pastes it (middleClickPaste). The clipboard
// is the page's (navigator.clipboard, the system clipboard), so the test
// puts back what it held.

const path = require('path');
const fs = require('fs');

const CHILD = path.join(path.dirname(process.execPath),
                        process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');
const SDLK = { LSHIFT: 0x400000E1, LCTRL: 0x400000E0, LALT: 0x400000E2, v: 118 };
const KMOD = { SHIFT: 0x0001, CTRL: 0x0040 };

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

function makeTerminal(cols, rows) {
    const t = document.createElement('terminal');
    t.setAttribute('cols', String(cols));
    t.setAttribute('rows', String(rows));
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

// The client point inside cell (col, row); `fx` the fraction across it.
function cellPoint(t, col, row, fx) {
    const r = t.getBoundingClientRect();
    const m = t.metrics;
    return [r.left + (col + (fx === undefined ? 0.25 : fx)) * m.cellWidth, r.top + (row + 0.5) * m.cellHeight];
}

function drag(t, from, to) {
    mouseDown(...cellPoint(t, ...from), 0);
    mouseMove(...cellPoint(t, ...to));
    mouseUp(...cellPoint(t, ...to), 0);
    advanceTime(16);
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = makeTerminal(24, 4);
    t.feed('alpha beta gamma\r\nsecond line here\r\nthird row text');
    waitFor(() => t.frameText().includes('third row'), 'the text is shown');

    // Character drag: from the left half of "a" to the right half of the
    // fifth cell selects exactly "alpha".
    drag(t, [0, 0, 0.2], [4, 0, 0.8]);
    assert(t.selectionText() === 'alpha', 'drag selects by character: ' + JSON.stringify(t.selectionText()));
    const sel = t.selection;
    assert(sel && sel.startCol === 0 && sel.endCol === 5 && sel.endRow === sel.startRow,
           'selection range ' + JSON.stringify(sel));

    // Across lines.
    drag(t, [6, 0, 0.2], [5, 1, 0.8]);
    assert(t.selectionText() === 'beta gamma\nsecond', 'a drag over two lines: ' + JSON.stringify(t.selectionText()));

    // Shift+click extends what is selected.
    drag(t, [0, 0, 0.2], [4, 0, 0.8]);
    keyDown(SDLK.LSHIFT, 0, KMOD.SHIFT);
    mouseDown(...cellPoint(t, 9, 0, 0.8), 0);
    mouseUp(...cellPoint(t, 9, 0, 0.8), 0);
    keyUp(SDLK.LSHIFT, 0, 0);
    advanceTime(16);
    assert(t.selectionText() === 'alpha beta', 'Shift+click extends: ' + JSON.stringify(t.selectionText()));

    // A click that does not move clears it.
    click(...cellPoint(t, 2, 2));
    advanceTime(16);
    assert(t.selection === null && t.selectionText() === '', 'a click clears the selection');

    // Double click: a word. Triple click: the line.
    click(...cellPoint(t, 2, 1));
    click(...cellPoint(t, 2, 1));
    advanceTime(16);
    assert(t.selectionText() === 'second', 'double click selects the word: ' + JSON.stringify(t.selectionText()));
    advanceTime(1000);
    wallSleep(600);  // past the double-click interval
    click(...cellPoint(t, 3, 2));
    click(...cellPoint(t, 3, 2));
    click(...cellPoint(t, 3, 2));
    advanceTime(16);
    assert(t.selectionText() === 'third row text', 'triple click selects the line: ' + JSON.stringify(t.selectionText()));
    wallSleep(600);

    // Alt+drag: a block.
    keyDown(SDLK.LALT, 0, 0x0100);
    drag(t, [0, 0, 0.2], [4, 1, 0.8]);
    keyUp(SDLK.LALT, 0, 0);
    assert(t.selectionText() === 'alpha\nsecon', 'Alt+drag selects a block: ' + JSON.stringify(t.selectionText()));

    // The pointer over text is an I-beam.
    mouseMove(...cellPoint(t, 1, 1));
    assert(currentCursor() === 'text', 'I-beam over the text: ' + currentCursor());

    // The page may keep a press for itself.
    t.clearSelection();
    const keep = (e) => e.preventDefault();
    t.addEventListener('mousedown', keep);
    drag(t, [0, 0, 0.2], [4, 0, 0.8]);
    assert(t.selection === null, 'a cancelled mousedown selects nothing');
    t.removeEventListener('mousedown', keep);

    // Copy-on-select puts the selection on the clipboard, and a middle click
    // pastes it into the program.
    if (fs.existsSync(CHILD)) {
        const clipboardBefore = navigator.clipboard.__read();
        t.options = { copyOnSelect: true, middleClickPaste: true };
        assert(t.options.copyOnSelect === true && t.options.middleClickPaste === true, 'options read back');
        drag(t, [11, 0, 0.2], [15, 0, 0.8]);
        assert(t.selectionText() === 'gamma', 'selected "gamma"');

        const k = makeTerminal(30, 4);
        k.options = { middleClickPaste: true };
        k.spawn({ command: CHILD, args: ['keys', '10'] });
        waitFor(() => k.screenText().includes('READY'), 'READY');
        k.focus();
        advanceTime(16);
        keyDown(SDLK.v, 0, KMOD.CTRL | KMOD.SHIFT);     // Ctrl+Shift+V: the clipboard
        keyUp(SDLK.v, 0, KMOD.CTRL | KMOD.SHIFT);
        mouseDown(...cellPoint(k, 5, 2), 1);            // middle click
        mouseUp(...cellPoint(k, 5, 2), 1);
        waitFor(() => k.screenText().includes('KEYS:'), 'the child read the pastes');
        const hex = 'KEYS:' + '67616d6d61' + '67616d6d61';
        assert(k.screenText().includes(hex), 'pasted "gamma" twice: ' + JSON.stringify(k.screenText()));
        navigator.clipboard.__write(clipboardBefore);
    }
}
