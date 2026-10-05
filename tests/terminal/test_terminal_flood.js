// <terminal>: a flood of output does not stall frames and loses nothing.
//
// pty_child (`flood N`) writes N bytes of numbered lines as fast as the PTY
// takes them, then FLOOD-DONE. The parser runs on its own thread and
// publishes at most one frame per frame taken, so the main thread's frame
// (pump, layout, paint, composite, readback) stays within budget however
// fast the output comes; the final screen must be exactly the last lines.

const path = require('path');
const fs = require('fs');

const CHILD = path.join(path.dirname(process.execPath),
                        process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');
const BYTES = 24 * 1024 * 1024;

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not built beside bro-headless (BRO_BUILD_TESTS)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '100');
    t.setAttribute('rows', '30');
    document.body.appendChild(t);
    flush();
    advanceTime(16);

    // One frame: the step (pump, layout, record) and a full composite with
    // readback, timed apart.
    function frame(stepMs, renderMs) {
        const a = perf.now();
        advanceTime(16);
        const b = perf.now();
        getPixels(0, 0, 1, 1);
        stepMs.push(b - a);
        renderMs.push(perf.now() - b);
    }
    const idleStep = [], idleRender = [];
    t.feed('idle frame text\r\n');
    for (let i = 0; i < 40; ++i) frame(idleStep, idleRender);

    const t0 = perf.now();
    t.spawn({ command: CHILD, args: ['flood', String(BYTES)] });
    const stepMs = [], renderMs = [];
    let frames = 0;
    while (t.running && perf.now() - t0 < 180000) {
        frame(stepMs, renderMs);
        ++frames;
    }
    const frameMs = stepMs.map((s, i) => s + renderMs[i]);
    const secs = (perf.now() - t0) / 1000;
    assert(!t.running, 'the flood finished');
    for (let i = 0; i < 20; ++i) advanceTime(16);

    const pct = (arr, p) => {
        const s = arr.slice().sort((x, y) => x - y);
        return s[Math.min(s.length - 1, Math.floor(s.length * p))];
    };
    const fmt = (arr) => 'p50 ' + pct(arr, 0.5).toFixed(2) + ' p95 ' + pct(arr, 0.95).toFixed(2) +
                         ' max ' + pct(arr, 1).toFixed(2);
    const mb = BYTES / (1024 * 1024);
    console.log('flood: ' + mb.toFixed(0) + ' MB in ' + secs.toFixed(2) + ' s = ' + (mb / secs).toFixed(1) +
                ' MB/s over ' + frames + ' frames');
    console.log('  idle  step ms ' + fmt(idleStep) + ' | render ms ' + fmt(idleRender));
    console.log('  flood step ms ' + fmt(stepMs) + ' | render ms ' + fmt(renderMs) + ' | frame ms ' + fmt(frameMs));
    // The terminal's share of a frame: what the main thread spends on the
    // step (pump, layout, recording the paint) must fit a 60 Hz frame
    // however fast output comes, and the composite must cost what an idle
    // one does (headless's composite + full readback is the fixed part).
    assert(pct(stepMs, 0.95) < 16.7, 'p95 step ' + pct(stepMs, 0.95).toFixed(2) + ' ms within a 60 Hz frame');
    assert(pct(renderMs, 0.95) < pct(idleRender, 0.95) * 1.5 + 4,
           'p95 render ' + pct(renderMs, 0.95).toFixed(2) + ' ms close to idle ' + pct(idleRender, 0.95).toFixed(2));
    // Frames kept coming: at least 10 per second of flood (a fast PTY, as on
    // Linux, finishes 24 MB in a fraction of a second, so no fixed count).
    assert(frames >= Math.max(3, Math.floor(secs * 10)),
           'frames kept coming during the flood (' + frames + ' in ' + secs.toFixed(2) + ' s)');

    // Exactly the last lines, nothing lost: the line numbers the byte count implies.
    let sent = 0, n = 0;
    for (;; ++n) {
        sent += ('flood line ' + n + ' ................................................\n').length;
        if (sent >= BYTES) break;
    }
    const want = [];
    for (let i = n - 27; i <= n; ++i) want.push('flood line ' + i + ' ................................................');
    want.push('FLOOD-DONE');
    const screen = t.screenText().split('\n');
    const tail = screen.slice(screen.length - want.length);
    assert(tail.join('\n') === want.join('\n'),
           'the final screen is the last lines:\n' + tail.slice(-3).join('\n') + '\nwant\n' + want.slice(-3).join('\n'));
    assert(t.frameText().includes('FLOOD-DONE'), 'and it is presented');
    assert(t.exitCode === 0, 'exit 0');
}
