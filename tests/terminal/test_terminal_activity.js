// <terminal>: the `activity` event, which tells the page the program produced
// output, so an app can mark a background tab busy without polling.
//
//   * output parsed fires it: a real child's, and feed()'s (which stands for
//     a program's); detail.bytes is what was parsed since the last one;
//   * it is coalesced: however much arrives (a flood, several writes in one
//     turn), at most one event per frame;
//   * nothing that is not output fires it: scrolling, selecting, searching,
//     theme and option changes;
//   * a persistent (bromux) session fires it when a frame from the server
//     changes the screen, with bytes null, and not for the page's own
//     scrolling.

const path = require('path');
const fs = require('fs');

const WIN = process.platform === 'win32';
const ENTER = WIN ? '\r' : '\n';
const CHILD = path.join(path.dirname(process.execPath), WIN ? 'bro_pty_child.exe' : 'bro_pty_child');
const SERVER = 'bro-js-activity-' + Date.now() % 1000000;

function waitFor(pred, what, ms) {
    const end = Date.now() + (ms || 20000);
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
    t.setAttribute('cols', String(cols || 40));
    t.setAttribute('rows', String(rows || 6));
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

// Record the element's activity events: { list, frames } where `frames`
// counts advanceTime steps taken through `step()`.
function recorder(t) {
    const r = { list: [], frames: 0 };
    t.addEventListener('activity', (e) => r.list.push(e.detail));
    r.step = (n) => {
        for (let i = 0; i < (n || 1); ++i) {
            advanceTime(16);
            ++r.frames;
        }
    };
    r.reset = () => { r.list.length = 0; r.frames = 0; };
    return r;
}

// Frames until no activity for `quiet` frames in a row (bounded).
function settle(t, r, quiet) {
    let calm = 0;
    const end = Date.now() + 20000;
    while (calm < (quiet || 30) && Date.now() < end) {
        const before = r.list.length;
        r.step();
        wallSleep(5);
        calm = r.list.length === before ? calm + 1 : 0;
    }
    assert(calm >= (quiet || 30), 'the terminal went quiet');
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    // ---- feed(): output without a process ---------------------------------------
    const t = makeTerminal();
    const r = recorder(t);
    r.step(3);
    assert(r.list.length === 0, 'no output, no activity: ' + JSON.stringify(r.list));

    t.feed('hello');
    r.step();
    assert(r.list.length === 1 && r.list[0].bytes === 5, 'feed() is output: ' + JSON.stringify(r.list));
    r.step(3);
    assert(r.list.length === 1, 'one event per burst, not per frame: ' + r.list.length);

    // Several writes in one turn: one event, with all their bytes.
    r.reset();
    t.feed('a\r\n');
    t.feed('bb\r\n');
    t.feed('ccc\r\n');
    r.step(2);
    assert(r.list.length === 1 && r.list[0].bytes === 12, 'coalesced in one frame: ' + JSON.stringify(r.list));

    // Nothing that is not output: history to scroll first.
    for (let i = 0; i < 30; ++i) t.feed('line ' + i + '\r\n');
    r.step(2);
    r.reset();
    t.scrollLines(-5);
    r.step(2);
    t.scrollToTop();
    r.step(2);
    t.select({ startRow: t.viewport.topRow, startCol: 0, endRow: t.viewport.topRow, endCol: 4 });
    r.step(2);
    t.selectAll();
    t.clearSelection();
    r.step(2);
    t.search('line');
    waitFor(() => t.searchStatus.complete, 'the search');
    t.searchNext();
    r.step(2);
    t.clearSearch();
    t.theme = { background: '#102030' };
    t.options = { minimumContrast: 3, scrollback: 500, cursorStyle: 'bar' };
    r.step(2);
    t.scrollToBottom();
    r.step(2);
    assert(r.list.length === 0, 'the page\'s own changes are not activity: ' + JSON.stringify(r.list));

    // ---- a real child ---------------------------------------------------------------
    if (fs.existsSync(CHILD)) {
        const c = makeTerminal(60, 10);
        const rc = recorder(c);
        c.spawn({ command: CHILD, args: ['print', 'from the child\\n'] });
        waitFor(() => c.screenText().includes('from the child') && !c.running, 'the child\'s output and exit');
        rc.step(2);
        assert(rc.list.length >= 1 && rc.list.every((d) => typeof d.bytes === 'number' && d.bytes > 0),
               'a child\'s output is activity: ' + JSON.stringify(rc.list));

        // A flood is at most one event a frame.
        const f = makeTerminal(80, 24);
        const rf = recorder(f);
        f.spawn({ command: CHILD, args: ['flood', String(2 * 1024 * 1024)] });
        const end = Date.now() + 60000;
        while (f.running && Date.now() < end) {
            rf.step();
            wallSleep(1);
        }
        assert(!f.running, 'the flood finished');
        rf.step(3);
        const total = rf.list.reduce((s, d) => s + d.bytes, 0);
        assert(rf.list.length >= 1 && rf.list.length <= rf.frames,
               'flood: ' + rf.list.length + ' events in ' + rf.frames + ' frames');
        assert(total >= 2 * 1024 * 1024, 'every byte counted once: ' + total);
        console.log('activity: flood of 2 MiB -> ' + rf.list.length + ' events over ' + rf.frames + ' frames');
    }

    // ---- a persistent session ----------------------------------------------------
    if (bro.terminal.persistentAvailable) {
        const p = makeTerminal(80, 10);
        p.style.width = '640px';
        p.style.height = '160px';
        const rp = recorder(p);
        p.spawn(WIN ? { command: 'cmd.exe', args: ['/d'], persistent: true, server: SERVER }
                    : { command: '/bin/sh', args: [], env: { PS1: '$ ' }, persistent: true, server: SERVER });
        waitFor(() => p.screenText().trim().length > 0, 'the prompt');
        settle(p, rp);
        assert(rp.list.length >= 1 && rp.list.every((d) => d.bytes === null),
               'the prompt was activity, bytes unknown: ' + JSON.stringify(rp.list.slice(0, 5)));

        // A command's output.
        rp.reset();
        p.write((WIN ? 'for /L %i in (1,1,30) do @echo act-%i' : 'i=1; while [ $i -le 30 ]; do echo act-$i; i=$((i+1)); done') + ENTER);
        const until = Date.now() + 20000;
        while (!/^act-30\s*$/m.test(p.screenText()) && Date.now() < until) {
            rp.step();
            wallSleep(5);
        }
        assert(/^act-30\s*$/m.test(p.screenText()), 'the command\'s output: ' + p.screenText());
        assert(rp.list.length >= 1 && rp.list.length <= rp.frames + 1 && rp.list.every((d) => d.bytes === null),
               'persistent output is activity: ' + rp.list.length + ' events in ' + rp.frames + ' frames');
        settle(p, rp);

        // The page's own scrolling is local to this element: none.
        rp.reset();
        p.scrollLines(-5);
        rp.step(5);
        p.scrollToBottom();
        rp.step(5);
        p.theme = { foreground: '#eeeeee' };
        rp.step(5);
        assert(rp.list.length === 0, 'persistent: scrolling is not activity: ' + JSON.stringify(rp.list));

        p.kill();
        waitFor(() => !p.running, 'the session closed');
        bro.terminal.killServer({ server: SERVER });
    }
}
