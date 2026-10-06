// <terminal>: synchronized output (DEC mode 2026).
//
// While a program holds an update open (CSI ? 2026 h) the screen keeps
// showing the last finished one: the half-drawn update is parsed but not
// presented. Ending it (CSI ? 2026 l) presents it at once; a program that
// never ends it is presented after a 200 ms timeout. frameText() reads what
// is presented, screenText() what has been parsed.

function waitFor(pred, what, ms) {
    const end = Date.now() + (ms || 5000);
    while (Date.now() < end) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(2);
    }
    assert(pred(), 'timed out waiting for ' + what);
    return false;
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = document.createElement('terminal');
    document.body.appendChild(t);
    flush();

    t.feed('stable');
    waitFor(() => t.frameText().includes('stable'), 'the first frame');

    // Held: parsed, not presented.
    t.feed('\x1b[?2026h\x1b[2J\x1b[Hhalf drawn');
    advanceTime(16);
    assert(t.screenText().includes('half drawn'), 'the update is parsed');
    assert(!t.frameText().includes('half drawn') && t.frameText().includes('stable'),
           'and not presented: ' + JSON.stringify(t.frameText()));

    // Ended: presented whole.
    t.feed(' and finished\x1b[?2026l');
    waitFor(() => t.frameText().includes('half drawn and finished'), 'the finished update');
    assert(!t.frameText().includes('stable'), 'the old screen is gone');

    // Back to back: the finished update is presented, its successor is not.
    t.feed('\x1b[?2026h\x1b[2J\x1b[Hframe A\x1b[?2026l\x1b[?2026h\x1b[2J\x1b[Hframe B partial');
    waitFor(() => t.frameText().includes('frame A'), 'frame A');
    assert(!t.frameText().includes('frame B'), 'frame B is still held');
    t.feed('\x1b[?2026l');
    waitFor(() => t.frameText().includes('frame B partial'), 'frame B');

    // Never ended: shown after the timeout.
    const t0 = perf.now();
    t.feed('\x1b[?2026h\x1b[2J\x1b[Hstuck update');
    waitFor(() => t.frameText().includes('stuck update'), 'the timed-out update');
    const held = perf.now() - t0;
    assert(held >= 150, 'held for the timeout first (' + held.toFixed(0) + ' ms)');
}
