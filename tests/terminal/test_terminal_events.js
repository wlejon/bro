// <terminal>: what the program says, as DOM events, each from a real escape
// sequence (fed through the parser, and from a real child for one of them).
//
//   titlechange  OSC 0 / 2         cwdchange  OSC 7          bell  BEL
//   notification OSC 9 / 777 / 99  progress   OSC 9;4        promptmark OSC 133
//   clipboardwrite / clipboardread OSC 52, under the element's clipboard policy
//
// and OSC 22's pointer shape, which the cursor over the element follows.

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

function makeTerminal() {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '40');
    t.setAttribute('rows', '4');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

// Feed `seq`, pump, and return the `type` events it raised (details).
function eventsOf(t, type, seq) {
    const got = [];
    const fn = (e) => got.push(e.detail);
    t.addEventListener(type, fn);
    t.feed(seq);
    advanceTime(16);
    advanceTime(16);
    t.removeEventListener(type, fn);
    return got;
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = makeTerminal();

    let e = eventsOf(t, 'titlechange', '\x1b]2;Hello title\x07');
    assert(e.length === 1 && e[0].title === 'Hello title', 'titlechange: ' + JSON.stringify(e));
    assert(t.title === 'Hello title', 'title property');

    // OSC 7: the path (decoded), the URI as sent and its host.
    e = eventsOf(t, 'cwdchange', '\x1b]7;file://host/tmp/some%20dir\x1b\\');
    assert(e.length === 1 && e[0].cwd === '/tmp/some dir' && e[0].uri === 'file://host/tmp/some%20dir' &&
           e[0].host === 'host', 'cwdchange: ' + JSON.stringify(e));
    assert(t.cwd === '/tmp/some dir', 'cwd property: ' + t.cwd);
    assert(t.cwdUri === 'file://host/tmp/some%20dir', 'cwdUri property: ' + t.cwdUri);
    e = eventsOf(t, 'cwdchange', '\x1b]7;file:///C:/Users/me/My%20Files\x07');
    assert(e.length === 1 && e[0].cwd === 'C:\\Users\\me\\My Files' && e[0].host === '',
           'a Windows drive: ' + JSON.stringify(e));
    assert(t.cwd === 'C:\\Users\\me\\My Files', 'cwd property, Windows drive: ' + t.cwd);

    e = eventsOf(t, 'bell', 'ding\x07');
    assert(e.length === 1, 'bell: ' + e.length);
    e = eventsOf(t, 'bell', '\x07\x07\x07');
    assert(e.length === 1, 'a burst of bells is one bell: ' + e.length);

    e = eventsOf(t, 'notification', '\x1b]9;Build finished\x07');
    assert(e.length === 1 && e[0].source === 'osc9' && (e[0].body + e[0].title).includes('Build finished'),
           'OSC 9 notification: ' + JSON.stringify(e));
    e = eventsOf(t, 'notification', '\x1b]777;notify;The title;The body\x07');
    assert(e.length === 1 && e[0].source === 'osc777' && e[0].title === 'The title' && e[0].body === 'The body',
           'OSC 777 notification: ' + JSON.stringify(e));
    e = eventsOf(t, 'notification', '\x1b]99;i=7:d=0;Kitty title\x1b\\\x1b]99;i=7:p=body;Kitty body\x1b\\');
    assert(e.length === 1 && e[0].source === 'osc99' && e[0].id === '7' && e[0].title === 'Kitty title' &&
           e[0].body === 'Kitty body', 'OSC 99 notification: ' + JSON.stringify(e));

    e = eventsOf(t, 'progress', '\x1b]9;4;1;42\x07');
    assert(e.length === 1 && e[0].state === 'normal' && e[0].value === 42, 'progress: ' + JSON.stringify(e));
    e = eventsOf(t, 'progress', '\x1b]9;4;2;80\x07');
    assert(e.length === 1 && e[0].state === 'error' && e[0].value === 80, 'progress error: ' + JSON.stringify(e));
    e = eventsOf(t, 'progress', '\x1b]9;4;0\x07');
    assert(e.length === 1 && e[0].state === 'none', 'progress cleared: ' + JSON.stringify(e));

    e = eventsOf(t, 'promptmark', '\r\n\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07out\r\n\x1b]133;D;3\x07');
    assert(e.length === 4 && e.map((m) => m.mark).join('') === 'ABCD' && e[3].exitCode === 3,
           'promptmark A B C D;3: ' + JSON.stringify(e));
    const cmds = t.commands;
    assert(cmds.length === 1 && cmds[0].exitCode === 3 && cmds[0].finished && cmds[0].output !== null,
           'commands: ' + JSON.stringify(cmds));

    // OSC 22: the pointer shape over the element.
    t.feed('\x1b]22;crosshair\x07');
    advanceTime(16);
    const r = t.getBoundingClientRect();
    mouseMove(r.left + 20, r.top + 10);
    assert(t.pointerShape === 'crosshair' && currentCursor() === 'crosshair',
           'OSC 22 sets the pointer: ' + t.pointerShape + ' / ' + currentCursor());
    t.feed('\x1b]22;\x07');
    advanceTime(16);
    assert(currentCursor() === 'text', 'an empty OSC 22 returns to the I-beam: ' + currentCursor());

    // OSC 52, write-only (the default): writes reach the page, reads do not.
    // They reach the system clipboard too (the page's): put it back after.
    const clipboardBefore = navigator.clipboard.__read();
    e = eventsOf(t, 'clipboardwrite', '\x1b]52;c;aGVsbG8=\x07');
    assert(e.length === 1 && e[0].text === 'hello' && e[0].selection === 'c', 'clipboardwrite: ' + JSON.stringify(e));
    e = eventsOf(t, 'clipboardread', '\x1b]52;c;?\x07');
    assert(e.length === 0, 'write-only: no clipboardread');
    t.options = { clipboard: 'deny' };
    e = eventsOf(t, 'clipboardwrite', '\x1b]52;c;aGVsbG8=\x07');
    assert(e.length === 0, 'deny: no clipboardwrite');
    let threw = false;
    try { t.options = { clipboard: 'everything' }; } catch (err) { threw = err instanceof TypeError; }
    assert(threw, 'a bad clipboard policy throws TypeError');

    if (fs.existsSync(CHILD)) {
        // A real child's title.
        const c = makeTerminal();
        const titles = [];
        c.addEventListener('titlechange', (ev) => titles.push(ev.detail.title));
        c.spawn({ command: CHILD, args: ['print', '\\e]2;From the child\\e\\'] });
        waitFor(() => titles.includes('From the child'), 'the child\'s title');

        // Read-write: a read is answered from the clipboard by default, by
        // the page when it cancels the event, and refused by denyClipboard.
        const rw = makeTerminal();
        rw.options = { clipboard: 'read-write' };
        const answer1 = '\x1b]52;c;aGVsbG8=\x07';   // "hello", from the clipboard
        const answer2 = '\x1b]52;c;eHl6\x07';       // "xyz", from the page
        rw.spawn({ command: CHILD, args: ['keys', String(answer1.length + answer2.length)] });
        waitFor(() => rw.screenText().includes('READY'), 'READY');
        e = eventsOf(rw, 'clipboardwrite', '\x1b]52;c;aGVsbG8=\x07');  // the clipboard now holds "hello"
        assert(e.length === 1, 'read-write: writes still reach the page');
        e = eventsOf(rw, 'clipboardread', '\x1b]52;c;?\x07');
        assert(e.length === 1 && typeof e[0].id === 'number' && e[0].selection === 'c',
               'clipboardread: ' + JSON.stringify(e));
        const reads = [];
        rw.addEventListener('clipboardread', (ev) => { reads.push(ev.detail.id); ev.preventDefault(); });
        rw.feed('\x1b]52;c;?\x07');
        waitFor(() => reads.length === 1, 'the second read');
        assert(rw.answerClipboard(reads[0], 'xyz') === true, 'answerClipboard');
        rw.feed('\x1b]52;c;?\x07');
        waitFor(() => reads.length === 2, 'the third read');
        assert(rw.denyClipboard(reads[1]) === true, 'denyClipboard');
        // Windows' ConPTY parses the input it is given and drops OSC sequences
        // there (a raw write() of one never reaches the child either), so the
        // answers can only be read back on a POSIX pty.
        if (process.platform !== 'win32') {
            waitFor(() => rw.screenText().replace(/\n/g, '').includes('KEYS:'), 'the child read the answers');
            const hex = Array.from(answer1 + answer2, (ch) => ch.charCodeAt(0).toString(16).padStart(2, '0')).join('');
            assert(rw.screenText().replace(/\n/g, '').includes('KEYS:' + hex),
                   'the answers: ' + JSON.stringify(rw.screenText()));
        }
    }
    navigator.clipboard.__write(clipboardBefore);
}
