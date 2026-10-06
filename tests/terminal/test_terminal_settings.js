// <terminal>: what the page reads of the program's settings and what it sets
// for it.
//
//   * `palette`: the theme with the program's OSC 4 / 10 / 11 / 12 over it
//     (`theme` stays the base), back to the theme on OSC 104 / 110 / 111 / 112;
//   * `bracketedPaste`: mode 2004, as the program set it;
//   * options.scrollback: the history kept, applied at once;
//   * options.cursorStyle / cursorBlink: the cursor DECSCUSR 0 returns to,
//     applied at once, not undoing the program's own style otherwise;
//   * the clipboard is the page's: navigator.clipboard sees a terminal copy,
//     and a terminal paste sends what navigator.clipboard wrote;
//   * the same reads and the cursor default on a persistent (bromux) session.
//
// The program settings come from a real child (bro_pty_child print), through
// the PTY (ConPTY on Windows) or the bromux server.

const path = require('path');
const fs = require('fs');

const WIN = process.platform === 'win32';
const ENTER = WIN ? '\r' : '\n';
const CHILD = path.join(path.dirname(process.execPath), WIN ? 'bro_pty_child.exe' : 'bro_pty_child');
const SERVER = 'bro-js-settings-' + Date.now() % 1000000;
const SDLK = { c: 99, v: 118 };
const KMOD = { SHIFT: 0x0001, CTRL: 0x0040 };

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
    t.setAttribute('cols', String(cols || 60));
    t.setAttribute('rows', String(rows || 8));
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

// Run bro_pty_child print `text` (\e, \a, \n expanded by the child) in a new
// terminal, and wait for it to finish.
function printed(text, setup) {
    const t = makeTerminal();
    if (setup) setup(t);
    t.spawn({ command: CHILD, args: ['print', text + 'PRINTED\\n'] });
    waitFor(() => t.screenText().includes('PRINTED') && !t.running, 'the child\'s output');
    advanceTime(16);
    return t;
}

function throws(fn) {
    try { fn(); } catch (e) { return e instanceof TypeError; }
    return false;
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not built beside bro-headless (BRO_BUILD_TESTS)');
} else {
    const THEME = { background: '#101010', foreground: '#c0c0c0', cursor: '#808080', ansi: [, '#aa0000'] };

    // ---- palette ---------------------------------------------------------------------
    {
        const t = printed('\\e]11;rgb:12/34/56\\a\\e]10;rgb:fe/dc/ba\\a\\e]12;rgb:00/ff/00\\a\\e]4;1;rgb:ff/80/00\\a',
                          (t) => { t.theme = THEME; });
        let p = t.palette;
        assert(p && p.ansi.length === 256, 'palette has all 256 entries');
        assert(p.background === '#123456' && p.foreground === '#fedcba' && p.cursor === '#00ff00' &&
               p.ansi[1] === '#ff8000', 'the program\'s colours: ' + JSON.stringify([p.background, p.foreground, p.cursor, p.ansi[1]]));
        assert(p.ansi[2] === t.theme.ansi[2], 'entries the program left are the theme\'s');
        const th = t.theme;
        assert(th.background === '#101010' && th.ansi[1] === '#aa0000', 'theme stays the base: ' + th.background);
        // Resets return to the theme.
        t.feed('\x1b]111\x07\x1b]104;1\x07');
        advanceTime(16);
        p = t.palette;
        assert(p.background === '#101010' && p.ansi[1] === '#aa0000' && p.foreground === '#fedcba',
               'OSC 111 / 104;1 reset just those: ' + JSON.stringify([p.background, p.ansi[1], p.foreground]));
    }

    // ---- bracketedPaste ----------------------------------------------------------------
    {
        const t = printed('\\e[?2004h');
        assert(t.bracketedPaste === true, 'the program turned bracketed paste on');
        t.feed('\x1b[?2004l');
        advanceTime(16);
        assert(t.bracketedPaste === false, 'and off');
        const fresh = makeTerminal();
        assert(fresh.bracketedPaste === false, 'off by default');
    }

    // ---- options.scrollback -----------------------------------------------------------
    {
        const t = makeTerminal(40, 5);
        assert(t.options.scrollback === 10000, 'default scrollback: ' + t.options.scrollback);
        t.spawn({ command: CHILD, args: ['flood', '20000'] });
        waitFor(() => !t.running, 'the flood');
        advanceTime(16);
        const before = t.viewport.historyRows;
        assert(before > 300, 'history from the flood: ' + before);
        t.options = { scrollback: 100 };
        advanceTime(16);
        assert(t.options.scrollback === 100, 'scrollback reads back');
        // History is dropped a whole line at a time, so a wrapped line at the
        // cut can leave it a row or two short of the limit.
        const held = t.viewport.historyRows;
        assert(held <= 100 && held >= 95, 'lowered: history cut at once: ' + held);
        assert(t.scrollbackText().split('\n').length === held, 'scrollbackText agrees');
        t.feed('more\r\n'.repeat(150));
        advanceTime(16);
        assert(t.viewport.historyRows === 100, 'held to it as output continues: ' + t.viewport.historyRows);
        t.options = { scrollback: 0 };
        advanceTime(16);
        assert(t.viewport.historyRows === 0 && t.scrollbackText() === '', 'zero keeps none');
        assert(throws(() => { t.options = { scrollback: -1 }; }), 'negative throws TypeError');
        assert(throws(() => { t.options = { scrollback: 2e6 }; }), 'too large throws TypeError');
    }

    // ---- options.cursorStyle / cursorBlink ----------------------------------------
    {
        const t = makeTerminal();
        let c = t.cursor;
        assert(t.options.cursorStyle === 'block' && t.options.cursorBlink === true && c.shape === 'block' && c.blink,
               'block and blinking by default: ' + JSON.stringify(c));
        t.options = { cursorStyle: 'bar', cursorBlink: false };
        c = t.cursor;
        assert(c.shape === 'bar' && !c.blink, 'applied at once: ' + JSON.stringify(c));
        assert(throws(() => { t.options = { cursorStyle: 'beam' }; }), 'an unknown style throws TypeError');

        // A program's own style holds until it asks for the default back.
        t.spawn({ command: CHILD, args: ['keys', '1'] });
        waitFor(() => t.screenText().includes('READY'), 'READY');
        t.feed('\x1b[3 q');
        c = t.cursor;
        assert(c.shape === 'underline' && c.blink, 'DECSCUSR 3: ' + JSON.stringify(c));
        t.options = { wheelLines: 5, cursorStyle: 'bar', cursorBlink: false };  // the same default: no change
        assert(t.cursor.shape === 'underline', 'other options leave the program\'s style: ' + t.cursor.shape);
        t.feed('\x1b[0 q');
        c = t.cursor;
        assert(c.shape === 'bar' && !c.blink, 'DECSCUSR 0 returns to the option: ' + JSON.stringify(c));
        t.write('q');
        waitFor(() => !t.running, 'the child to exit');

        // From a real child: its style, then the default back.
        const u = printed('\\e[4 q', (u) => { u.options = { cursorStyle: 'underline', cursorBlink: true }; });
        assert(u.cursor.shape === 'underline' && !u.cursor.blink, 'the child\'s DECSCUSR 4: ' + JSON.stringify(u.cursor));
        const v = printed('\\e[2 q\\e[0 q', (v) => { v.options = { cursorStyle: 'bar' }; });
        assert(v.cursor.shape === 'bar' && v.cursor.blink, 'the child\'s DECSCUSR 0: ' + JSON.stringify(v.cursor));
    }

    // ---- the clipboard is the page's ------------------------------------------------
    {
        const saved = navigator.clipboard.__read();
        // A terminal copy reaches navigator.clipboard.
        const t = makeTerminal(40, 4);
        t.feed('copy me please\r\n');
        advanceTime(16);
        t.select({ startRow: t.viewport.screenTopRow, startCol: 0, endRow: t.viewport.screenTopRow, endCol: 7 });
        t.focus();
        advanceTime(16);
        keyDown(SDLK.c, 0, KMOD.CTRL | KMOD.SHIFT);
        keyUp(SDLK.c, 0, KMOD.CTRL | KMOD.SHIFT);
        advanceTime(16);
        assert(navigator.clipboard.__read() === 'copy me', 'navigator.clipboard sees the copy: ' +
               JSON.stringify(navigator.clipboard.__read()));

        // What navigator.clipboard wrote is what a terminal pastes.
        const k = makeTerminal(40, 4);
        k.spawn({ command: CHILD, args: ['keys', '9'] });
        waitFor(() => k.screenText().includes('READY'), 'READY');
        navigator.clipboard.__write('from page');
        k.focus();
        advanceTime(16);
        keyDown(SDLK.v, 0, KMOD.CTRL | KMOD.SHIFT);
        keyUp(SDLK.v, 0, KMOD.CTRL | KMOD.SHIFT);
        waitFor(() => k.screenText().includes('KEYS:'), 'the child read the paste');
        assert(k.screenText().includes('KEYS:' + '66726f6d2070616765'), 'pasted "from page": ' + JSON.stringify(k.screenText()));

        // And an OSC 52 write the page lets through.
        t.feed('\x1b]52;c;b3NjIDUy\x07');  // "osc 52"
        advanceTime(16);
        advanceTime(16);
        assert(navigator.clipboard.__read() === 'osc 52', 'OSC 52 reaches navigator.clipboard: ' +
               JSON.stringify(navigator.clipboard.__read()));
        navigator.clipboard.__write(saved);
    }

    // ---- a persistent session -----------------------------------------------------------
    if (bro.terminal.persistentAvailable) {
        const p = makeTerminal(80, 10);
        p.style.width = '640px';
        p.style.height = '160px';
        p.theme = THEME;
        p.options = { cursorStyle: 'bar', cursorBlink: false, scrollback: 50 };
        p.spawn(WIN ? { command: 'cmd.exe', args: ['/d'], persistent: true, server: SERVER }
                    : { command: '/bin/sh', args: [], env: { PS1: '$ ' }, persistent: true, server: SERVER });
        waitFor(() => p.screenText().trim().length > 0, 'the prompt');
        assert(p.cursor.shape === 'bar' && !p.cursor.blink, 'the element\'s default cursor: ' + JSON.stringify(p.cursor));
        assert(p.palette.background === '#101010', 'the theme before the program changes it');
        assert(p.bracketedPaste === false, 'no bracketed paste yet');
        const q = WIN ? '"' : '\'';
        let n = 0;
        const runAndWait = (text) => {
            ++n;
            p.write(q + CHILD + q + ' print ' + q + text + 'DONE-' + n + '\\n' + q + ENTER);
            const tag = 'DONE-' + n;
            waitFor(() => new RegExp('^' + tag + '\\s*$', 'm').test(p.screenText()), tag);
            for (let i = 0; i < 5; ++i) { advanceTime(16); wallSleep(10); }
        };
        runAndWait('\\e]11;rgb:12/34/56\\a\\e]4;1;rgb:ff/80/00\\a\\e[?2004h\\e[3 q');
        let pal = p.palette;
        assert(pal.background === '#123456' && pal.ansi[1] === '#ff8000', 'persistent palette: ' +
               JSON.stringify([pal.background, pal.ansi[1]]));
        assert(pal.foreground === '#c0c0c0', 'the theme where the program changed nothing: ' + pal.foreground);
        assert(p.bracketedPaste === true, 'persistent bracketedPaste');
        assert(p.cursor.shape === 'underline' && p.cursor.blink, 'the program\'s cursor: ' + JSON.stringify(p.cursor));
        runAndWait('\\e[0 q\\e[?2004l');
        assert(p.cursor.shape === 'bar' && !p.cursor.blink, 'DECSCUSR 0 shows the element\'s default: ' +
               JSON.stringify(p.cursor));
        assert(p.bracketedPaste === false, 'persistent bracketedPaste off');
        p.options = { cursorStyle: 'underline' };
        advanceTime(16);
        assert(p.cursor.shape === 'underline' && !p.cursor.blink, 'a new default applies: ' + JSON.stringify(p.cursor));
        assert(p.options.scrollback === 50, 'the scrollback option reads back on a persistent session');

        p.kill();
        waitFor(() => !p.running, 'the session closed');
        bro.terminal.killServer({ server: SERVER });
    }
}
