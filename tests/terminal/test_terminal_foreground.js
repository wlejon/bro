// <terminal>: the foreground process, and foregroundchange.
//
// A real shell runs a chain of programs (shell -> pty_child nest 2 -> nest 1
// -> nest 0, each waiting for the next). foregroundProcess names the shell
// at its prompt; while the chain runs it names the program the user is
// talking to (POSIX: the foreground job's leader, `nest 2`, whose children
// share its process group; Windows: the youngest console program, `nest 0`);
// once the chain ends it is the shell again, and after the shell exits it is
// null. Each change fires foregroundchange with the new answer.

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
} else if (!fs.existsSync(CHILD)) {
    skipTest('bro_pty_child is not beside bro-headless');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '100');
    t.setAttribute('rows', '10');
    document.body.appendChild(t);
    flush();
    assert(t.foregroundProcess === null, 'no process before spawn');

    const changes = [];
    t.addEventListener('foregroundchange', (e) => changes.push(e.detail.process));
    const sh = shell();
    const enter = WIN ? '\r' : '\n';
    t.spawn({ command: sh.command, args: sh.args, env: WIN ? {} : { PS1: '$ ' } });
    const shellPid = t.pid;
    const shellName = path.basename(sh.command).toLowerCase();

    // At the prompt: the shell itself.
    waitFor(() => t.foregroundProcess && t.foregroundProcess.pid === shellPid, 'the shell in the foreground');
    const atPrompt = t.foregroundProcess;
    assert(atPrompt.name.toLowerCase() === shellName, 'the shell\'s name: ' + JSON.stringify(atPrompt));
    assert(atPrompt.commandLine.length > 0 && atPrompt.path.length > 0, 'command line and path: ' + JSON.stringify(atPrompt));
    waitFor(() => changes.length >= 1, 'a foregroundchange for the shell');
    assert(changes[changes.length - 1] && changes[changes.length - 1].pid === shellPid, 'the event names the shell');

    // shell -> child -> grandchild -> great-grandchild.
    t.write('"' + CHILD + '" nest 2' + enter);
    waitFor(() => /NEST-READY pid=\d+/.test(t.screenText()), 'the chain is running');
    const leaf = Number(/NEST-READY pid=(\d+)/.exec(t.screenText())[1]);
    let want, tail;
    if (WIN) {
        want = leaf;
        tail = 'nest 0';
    } else {
        want = Number(/pgid=(\d+)/.exec(t.screenText())[1]);
        tail = 'nest 2';
        assert(want !== leaf && want !== shellPid, 'the job leader is the shell\'s child, not the leaf');
    }
    waitFor(() => t.foregroundProcess && t.foregroundProcess.pid === want, 'the chain in the foreground', 10000);
    const running = t.foregroundProcess;
    assert(running.name === (WIN ? 'bro_pty_child.exe' : 'bro_pty_child'), 'the program\'s name: ' + JSON.stringify(running));
    assert(running.commandLine.endsWith(tail), 'its command line ends "' + tail + '": ' + JSON.stringify(running));
    waitFor(() => changes.some((p) => p && p.pid === want), 'a foregroundchange for the program');

    // The chain ends: the shell again.
    t.write('q' + enter);
    waitFor(() => t.foregroundProcess && t.foregroundProcess.pid === shellPid, 'the shell back in the foreground', 10000);
    waitFor(() => { const p = changes[changes.length - 1]; return p && p.pid === shellPid; },
            'a foregroundchange back to the shell');

    // The shell exits: nothing.
    let exited = false;
    t.addEventListener('exit', () => { exited = true; });
    t.write('exit' + enter);
    waitFor(() => exited, 'the shell exits');
    waitFor(() => t.foregroundProcess === null, 'no foreground process after exit');
    waitFor(() => changes[changes.length - 1] === null, 'a foregroundchange to null');
}
