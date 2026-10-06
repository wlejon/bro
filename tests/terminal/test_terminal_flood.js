// <terminal>: a flood of output does not stall frames and loses nothing.
//
// pty_child (`flood N`) writes N bytes of numbered lines as fast as the PTY
// takes them, then FLOOD-DONE. The parser runs on its own thread and
// publishes at most one frame per frame taken, so the main thread's frame
// (pump, layout, paint, composite, readback) stays within budget however
// fast the output comes; the final screen must be exactly the last lines.
//
// What is measured, and how. The guarantee is about the main thread's own
// work, so that is what the budgets are checked against: its CPU time
// (perf.threadCpuMs), not wall time. Wall time also counts the time other
// processes held the core, which on a loaded machine (a full test suite, a
// build) inflates every frame, the idle baseline included, by amounts that
// have nothing to do with the terminal. The idle baseline is taken both
// before and after the flood and the larger of the two used, so a change in
// load across the run cannot make the baseline unrepresentative either.
// Presentation keeping up is checked by what was presented (a new frame on
// most frames of the flood), not by a frame rate. Wall times are logged.

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
    // readback, timed apart, in main-thread CPU ms and in wall ms.
    function frame(m) {
        const w0 = perf.now(), c0 = perf.threadCpuMs();
        advanceTime(16);
        const w1 = perf.now(), c1 = perf.threadCpuMs();
        getPixels(0, 0, 1, 1);
        const w2 = perf.now(), c2 = perf.threadCpuMs();
        m.step.push(c1 - c0);
        m.render.push(c2 - c1);
        m.wallStep.push(w1 - w0);
        m.wallRender.push(w2 - w1);
    }
    const series = () => ({ step: [], render: [], wallStep: [], wallRender: [] });
    const idleBefore = series(), idleAfter = series();
    t.feed('idle frame text\r\n');
    for (let i = 0; i < 40; ++i) frame(idleBefore);

    const t0 = perf.now();
    t.spawn({ command: CHILD, args: ['flood', String(BYTES)] });
    const flood = series();
    let frames = 0, presented = 0, lastText = t.frameText();
    while (t.running && perf.now() - t0 < 180000) {
        frame(flood);
        ++frames;
        const text = t.frameText();
        if (text !== lastText) ++presented;
        lastText = text;
    }
    const secs = (perf.now() - t0) / 1000;
    assert(!t.running, 'the flood finished');
    for (let i = 0; i < 20; ++i) advanceTime(16);
    for (let i = 0; i < 40; ++i) frame(idleAfter);

    const pct = (arr, p) => {
        const s = arr.slice().sort((x, y) => x - y);
        return s[Math.min(s.length - 1, Math.floor(s.length * p))];
    };
    const fmt = (arr) => 'p50 ' + pct(arr, 0.5).toFixed(2) + ' p95 ' + pct(arr, 0.95).toFixed(2) +
                         ' max ' + pct(arr, 1).toFixed(2);
    const mb = BYTES / (1024 * 1024);
    console.log('flood: ' + mb.toFixed(0) + ' MB in ' + secs.toFixed(2) + ' s = ' + (mb / secs).toFixed(1) +
                ' MB/s over ' + frames + ' frames, ' + presented + ' presented a new frame');
    for (const [name, m] of [['idle before', idleBefore], ['flood', flood], ['idle after', idleAfter]]) {
        console.log('  ' + name + ' cpu  step ms ' + fmt(m.step) + ' | render ms ' + fmt(m.render));
        console.log('  ' + name + ' wall step ms ' + fmt(m.wallStep) + ' | render ms ' + fmt(m.wallRender));
    }
    // The terminal's share of a frame: the main thread's work on the step
    // (pump, layout, recording the paint) fits a 60 Hz frame however fast
    // output comes, and the composite costs what an idle one does
    // (headless's composite + full readback is the fixed part).
    const idleRender = Math.max(pct(idleBefore.render, 0.95), pct(idleAfter.render, 0.95));
    assert(pct(flood.step, 0.95) < 16.7,
           'p95 step ' + pct(flood.step, 0.95).toFixed(2) + ' CPU ms within a 60 Hz frame');
    assert(pct(flood.render, 0.95) < idleRender * 1.5 + 4,
           'p95 render ' + pct(flood.render, 0.95).toFixed(2) + ' CPU ms close to idle ' + idleRender.toFixed(2));
    // Presentation kept up: the flood's progress reached the screen on most
    // frames (the parser publishes for every frame taken), not only at the end.
    // A fast PTY (Linux) finishes in a few frames, hence the floor.
    if (frames >= 4) {
        assert(presented >= Math.floor(frames / 2),
               'a new frame was presented on most frames of the flood (' + presented + ' of ' + frames + ')');
    }

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
