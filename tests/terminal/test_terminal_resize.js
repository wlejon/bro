// <terminal>: a CSS size change reaches the child.
//
// The grid follows the content box: a new width/height becomes a new
// cols x rows, the PTY is resized (the child sees it in its window size),
// and the element fires `resize` with the new grid. bropty's pty_child
// (`size`) prints its window size after every byte it reads.

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

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not built beside bro-headless (BRO_BUILD_TESTS)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '40');
    t.setAttribute('rows', '10');
    t.style.font = '14px monospace';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    assert(t.cols === 40 && t.rows === 10, 'grid from attributes: ' + t.cols + 'x' + t.rows);

    const resizes = [];
    t.addEventListener('resize', (e) => resizes.push(e.detail));

    t.spawn({ command: CHILD, args: ['size'] });
    waitFor(() => t.screenText().includes('SIZE 10x40'), 'the initial size, ' + JSON.stringify(t.screenText()));

    // One cell's size, from the box the attributes gave.
    const r = t.getBoundingClientRect();
    const cellW = r.width / 40, cellH = r.height / 10;

    t.style.width = (cellW * 60 + 2) + 'px';
    t.style.height = (cellH * 15 + 2) + 'px';
    flush();
    advanceTime(16);
    advanceTime(16);
    assert(t.cols === 60 && t.rows === 15, 'grid follows the box: ' + t.cols + 'x' + t.rows);
    assert(resizes.length >= 1, 'a resize event fired');
    const last = resizes[resizes.length - 1];
    assert(last && last.cols === 60 && last.rows === 15, 'resize detail ' + JSON.stringify(last));

    t.write('x');
    waitFor(() => t.screenText().includes('SIZE 15x60'), 'the child to see 15x60, ' + JSON.stringify(t.screenText()));

    // Narrower again, through a class change.
    const style = document.createElement('style');
    style.textContent = '.narrow { width: ' + (cellW * 30 + 2) + 'px !important; }';
    document.head.appendChild(style);
    t.className = 'narrow';
    flush();
    advanceTime(16);
    advanceTime(16);
    assert(t.cols === 30, 'a stylesheet change resizes too: ' + t.cols);
    t.write('x');
    waitFor(() => t.screenText().includes('SIZE 15x30'), 'the child to see 15x30, ' + JSON.stringify(t.screenText()));

    t.write('q');
    waitFor(() => !t.running, 'the child to exit');
}
