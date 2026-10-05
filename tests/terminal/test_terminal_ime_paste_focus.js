// <terminal>: IME composition, bracketed paste and focus reporting.
//
// The composition is shown at the cursor and only its commit reaches the
// child. A paste is bracketed (ESC[200~ ... ESC[201~) once the program has
// set mode 2004, and focus changes are reported (ESC[I / ESC[O) once it has
// set mode 1004. The modes are set here by feeding the sequences into the
// emulator, as the program would by printing them.
//
// Windows' ConPTY sits between the terminal and the child and takes the
// focus reports for itself (they become console focus events, which a
// raw-mode reader never sees), so there the focus half is not checked; the
// bracketed paste passes through it unchanged.

const path = require('path');
const fs = require('fs');

const CHILD = path.join(path.dirname(process.execPath),
                        process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');
const WIN = process.platform === 'win32';

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

function hex(s) {
    return Array.from(new TextEncoder().encode(s), (b) => b.toString(16).padStart(2, '0')).join('');
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not built beside bro-headless (BRO_BUILD_TESTS)');
} else {
    const other = document.createElement('input');
    document.body.appendChild(other);
    const t = document.createElement('terminal');
    document.body.appendChild(t);
    flush();

    const events = [];
    for (const type of ['compositionstart', 'compositionupdate', 'compositionend', 'paste'])
        t.addEventListener(type, (e) => events.push(type));

    // The bytes this test sends: the commit, the paste, and (POSIX) the
    // brackets and the two focus reports.
    const commit = '日本';
    const pasted = 'pasted text';
    let expected = hex(commit) + '1b5b3230307e' + hex(pasted) + '1b5b3230317e';
    if (!WIN) expected += '1b5b4f' + '1b5b49';
    t.spawn({ command: CHILD, args: ['keys', String(expected.length / 2)] });
    waitFor(() => t.screenText().includes('READY'), 'READY');
    t.focus();
    advanceTime(16);

    // Composition: shown, not sent.
    imeCompose('にほ');
    advanceTime(16);
    assert(events.includes('compositionstart') && events.includes('compositionupdate'),
           'composition events: ' + events.join(','));
    const r = t.getBoundingClientRect();
    imeCommit(commit);
    advanceTime(16);
    assert(events.includes('compositionend'), 'compositionend on commit');

    // Bracketed paste once the program asks for it.
    t.feed('\x1b[?2004h');
    paste(pasted);
    assert(events.includes('paste'), 'the page sees the paste event');

    // Focus reports once the program asks for them.
    t.feed('\x1b[?1004h');
    advanceTime(16);
    other.focus();
    advanceTime(16);
    t.focus();
    advanceTime(16);

    waitFor(() => !t.running, 'the child to report and exit');
    const m = /KEYS:([0-9a-f]*)/.exec(t.screenText());
    assert(m, 'the child reported its input: ' + JSON.stringify(t.screenText()));
    if (m) assert(m[1] === expected, 'bytes received ' + m[1] + ', want ' + expected);
    assert(r.width > 0, 'the terminal was laid out');
}
