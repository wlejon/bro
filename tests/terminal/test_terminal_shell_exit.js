// <terminal>: a real shell runs in it, and exits are reported.
//
// The platform's shell (cmd.exe on Windows, bash or sh elsewhere) echoes a
// command's output into the screen; `exit N` ends it with `exit` carrying N.
// pty_child's `exit N` and a killed child report through the same event.

const path = require('path');
const fs = require('fs');

const WIN = process.platform === 'win32';
const CHILD = path.join(path.dirname(process.execPath), WIN ? 'bro_pty_child.exe' : 'bro_pty_child');

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

function shell() {
    if (WIN) return { command: process.env.COMSPEC || 'cmd.exe', args: [] };
    for (const sh of ['/bin/bash', '/usr/bin/bash', '/bin/sh'])
        if (fs.existsSync(sh)) return { command: sh, args: sh.endsWith('bash') ? ['--norc', '--noprofile'] : [] };
    return { command: '/bin/sh', args: [] };
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    // A shell, a command, its output, `exit 3`.
    const t = document.createElement('terminal');
    document.body.appendChild(t);
    flush();
    const exits = [];
    t.addEventListener('exit', (e) => exits.push(e.detail));
    const sh = shell();
    const env = WIN ? {} : { PS1: '$ ', TERM: 'xterm-256color' };
    t.spawn({ command: sh.command, args: sh.args, env });
    assert(t.running, 'the shell runs: ' + sh.command);
    t.focus();
    advanceTime(16);
    // Typed, the way a person would: text and Enter through the engine.
    textInput('echo bro-says-' + 'hello');
    keyDown(13);
    keyUp(13);
    waitFor(() => /^bro-says-hello\s*$/m.test(t.screenText()),
            'the command output on its own line: ' + JSON.stringify(t.screenText()));
    t.write('exit 3\r');
    waitFor(() => exits.length === 1, 'the exit event');
    assert(exits[0] && exits[0].exitCode === 3, 'exit detail ' + JSON.stringify(exits[0]));
    assert(t.exitCode === 3 && !t.running, 'exitCode 3, not running');
    assert(t.write('more') === false, 'writing after exit is refused');
    advanceTime(100);
    assert(exits.length === 1, 'exit fires once');

    if (fs.existsSync(CHILD)) {
        // A program's own status.
        const e = document.createElement('terminal');
        document.body.appendChild(e);
        flush();
        let code;
        e.addEventListener('exit', (ev) => { code = ev.detail.exitCode; });
        e.spawn({ command: CHILD, args: ['exit', '42'] });
        waitFor(() => code !== undefined, 'pty_child exit 42');
        assert(code === 42, 'exit code 42, got ' + code);

        // Killed: the event still comes, with whatever status the platform gives.
        const k = document.createElement('terminal');
        document.body.appendChild(k);
        flush();
        let killed = false;
        k.addEventListener('exit', () => { killed = true; });
        k.spawn({ command: CHILD, args: ['stubborn'] });
        waitFor(() => k.screenText().includes('READY'), 'stubborn READY');
        k.kill();
        waitFor(() => killed, 'the killed child\'s exit event', 30000);
        assert(!k.running, 'not running after kill');

        // Arguments, environment and working directory reach the child.
        const a = document.createElement('terminal');
        document.body.appendChild(a);
        flush();
        a.spawn({ command: CHILD, args: ['env', 'BRO_TERM_TEST'], env: { BRO_TERM_TEST: 'yes-it-is' } });
        waitFor(() => !a.running, 'env child');
        assert(a.screenText().includes('BRO_TERM_TEST=yes-it-is'), 'env reached the child: ' + a.screenText());

        // Spawning twice is refused.
        let threw = false;
        try { a.spawn({ command: CHILD, args: ['exit', '0'] }); } catch (err) { threw = true; }
        assert(threw, 'a second spawn throws');
    }

    // A command that does not exist throws.
    const bad = document.createElement('terminal');
    document.body.appendChild(bad);
    flush();
    let threw = false;
    try { bad.spawn({ command: path.join(bro.appDir, 'no-such-program-here') }); } catch (err) { threw = true; }
    if (!threw) {
        // POSIX forks first and reports the failed exec as the child's exit.
        let code;
        bad.addEventListener('exit', (ev) => { code = ev.detail.exitCode; });
        waitFor(() => code !== undefined, 'the failed exec to exit');
        assert(code !== 0, 'a missing program does not exit 0: ' + code);
    }
}
