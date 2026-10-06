// <terminal>: what a persistent (bromux) session carries beyond the screen,
// as a local one does: the foreground process and foregroundchange, feed(),
// OSC 133 command records with exit codes, the OSC 22 pointer shape, OSC 99's
// notification id and urgency, and inline images. Bytes go in through feed()
// (ConPTY consumes OSC 133 / 22 / 99 and kitty graphics a native program
// writes, so feed() is how a test reaches the server's terminal on every
// platform), on a bromux server private to this test.

const WIN = process.platform === 'win32';
const ENTER = WIN ? '\r' : '\n';
const SERVER = 'bro-js-extras-' + Date.now() % 1000000;

function waitFor(pred, what, ms) {
    const end = Date.now() + (ms || 20000);
    while (Date.now() < end) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(5);
    }
    assert(pred(), 'timed out waiting for ' + (typeof what === 'function' ? what() : what));
    return false;
}

function makeTerminal() {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '80');
    t.setAttribute('rows', '12');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block; width: 640px; height: 192px';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

const shell = WIN ? { command: 'cmd.exe', args: ['/d'] } : { command: '/bin/sh', args: [], env: { PS1: '$ ' } };
const shellName = WIN ? /^cmd\.exe$/i : /sh$/;

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!bro.terminal.persistentAvailable) {
    skipTest('persistent sessions are compiled out (no bromux)');
} else {
    const t = makeTerminal();
    const fgEvents = [];
    t.addEventListener('foregroundchange', (e) => fgEvents.push(e.detail.process));
    t.spawn(Object.assign({ persistent: true, server: SERVER, name: 'extras' }, shell));
    assert(t.sessionId > 0, 'persistent');

    // ---- the foreground process --------------------------------------------------
    waitFor(() => t.foregroundProcess && shellName.test(t.foregroundProcess.name),
            'the shell as the foreground process: ' + JSON.stringify(t.foregroundProcess));
    assert(t.foregroundProcess.pid === t.pid, 'the shell is the session program: ' +
           JSON.stringify(t.foregroundProcess) + ' pid ' + t.pid);
    assert(t.foregroundProcess.path.length > 0, 'with its executable');
    waitFor(() => fgEvents.length > 0, 'a foregroundchange');
    assert(fgEvents[fgEvents.length - 1] && fgEvents[fgEvents.length - 1].pid === t.pid, 'the event names it');

    // ---- feed(): into the server's terminal -------------------------------------
    // After the shell's first prompt: ConPTY's first paint erases the screen
    // (and with it what was fed before it, command records included).
    waitFor(() => (WIN ? />\s*$/ : /\$\s*$/).test(t.screenText()), () => 'the prompt: ' + JSON.stringify(t.screenText()));
    t.feed('\x1b]22;pointer\x07FED-TEXT\r\n');
    waitFor(() => /FED-TEXT/.test(t.screenText()), 'fed text on the screen: ' + t.screenText());
    waitFor(() => t.pointerShape === 'pointer', 'the OSC 22 pointer shape: ' + JSON.stringify(t.pointerShape));

    // ---- OSC 133 command records -----------------------------------------------
    const marks = [];
    t.addEventListener('promptmark', (e) => marks.push(e.detail.mark));
    t.feed('\x1b]133;A\x07$ \x1b]133;B\x07make all\r\n\x1b]133;C\x07' + 'building\r\n\x1b]133;D;3\x07' +
           '\x1b]133;A\x07$ \x1b]133;B\x07true\r\n\x1b]133;C;cmdline=true\x07\x1b]133;D;0\x07\x1b]133;A\x07$ ');
    waitFor(() => t.commands.length >= 3, () => 'three command records: ' + JSON.stringify(t.commands) +
            ' screen ' + JSON.stringify(t.screenText()) + ' marks ' + marks.join(''));
    const cmds = t.commands.slice(-3);
    assert(cmds[0].finished && cmds[0].exitCode === 3 && cmds[0].commandLine === 'make all',
           'the first command, its exit code and the line typed: ' + JSON.stringify(cmds[0]));
    assert(cmds[1].exitCode === 0 && cmds[1].commandLine === 'true', 'the second: ' + JSON.stringify(cmds[1]));
    assert(!cmds[2].finished && cmds[2].exitCode === null, 'the prompt waiting: ' + JSON.stringify(cmds[2]));
    assert(cmds[0].output && cmds[0].end && cmds[0].output.row < cmds[0].end.row, 'positions: ' + JSON.stringify(cmds[0]));
    assert(t.textInRange({ startRow: cmds[0].output.row, startCol: 0, endRow: cmds[0].end.row, endCol: 0 })
               .includes('building'), 'the records name rows of this screen');
    waitFor(() => marks.join('').includes('ABCD'), 'promptmark events: ' + marks.join(''));

    // ---- OSC 99 notification fields --------------------------------------------
    const notes = [];
    t.addEventListener('notification', (e) => notes.push(e.detail));
    t.feed('\x1b]99;i=job1:u=2:d=0;Build\x1b\\\x1b]99;i=job1:p=body;finished\x1b\\');
    waitFor(() => notes.length > 0, 'the notification');
    const n = notes[0];
    assert(n.id === 'job1' && n.urgency === 2 && n.source === 'osc99' && n.title === 'Build' && n.body === 'finished',
           'id, urgency and source cross bromux: ' + JSON.stringify(n));

    // ---- inline images -----------------------------------------------------------
    const px = btoa(String.fromCharCode(...[255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255]));
    t.feed('\x1b_Ga=T,f=32,s=2,v=2,c=6,r=3,i=11,q=2;' + px + '\x1b\\\r\n\r\n\r\n');
    waitFor(() => t.images.count === 1 && t.images.placements === 1 && t.images.bytes === 16,
            'the image on the element: ' + JSON.stringify(t.images));
    t.feed('\x1b_Ga=d,d=A,q=2\x1b\\');
    waitFor(() => t.images.placements === 0, 'deleted: ' + JSON.stringify(t.images));

    // ---- a second element attached later gets the same state ---------------------
    const u = makeTerminal();
    u.attach(t.sessionId, { server: SERVER });
    waitFor(() => u.foregroundProcess && u.foregroundProcess.pid === t.pid, 'the foreground on attach');
    waitFor(() => u.commands.length === t.commands.length, 'the command records on attach');
    assert(u.pointerShape === 'pointer', 'the pointer shape on attach');

    // ---- the program exits: nothing owns the terminal ------------------------------
    let exited = false;
    t.addEventListener('exit', () => { exited = true; });
    t.write('exit' + ENTER);
    waitFor(() => exited, 'exit');
    waitFor(() => t.foregroundProcess === null, 'no foreground process after exit');
    assert(fgEvents[fgEvents.length - 1] === null, 'foregroundchange to null');

    assert(bro.terminal.killServer({ server: SERVER }), 'killServer');
}
