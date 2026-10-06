// <terminal>: persistent sessions (spawn({persistent}), attach, detach,
// bro.terminal.sessions) on a bromux server private to this test.
//
//   * a shell started persistent survives its element's removal (which
//     detaches it), and a new element attached by id shows the same screen
//     and the same scrollback;
//   * it survives a page reload (a child bro-headless whose app reloads);
//   * two elements attached to one session share its input and output;
//   * the program's exit reaches every element attached;
//   * closeSession / killServer clean up.

const path = require('path');
const cp = require('child_process');

const WIN = process.platform === 'win32';
const ENTER = WIN ? '\r' : '\n';
const SERVER = 'bro-js-test-' + Date.now() % 1000000;

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

function makeTerminal() {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '80');
    t.setAttribute('rows', '10');
    t.style.cssText = 'font: 16px monospace; padding: 0; border: 0; display: block; width: 640px; height: 160px';
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    return t;
}

function shell() {
    return WIN ? { command: 'cmd.exe', args: ['/d'] } : { command: '/bin/sh', args: [], env: { PS1: '$ ' } };
}

function session(id) {
    return bro.terminal.sessions({ server: SERVER }).find((s) => s.id === id);
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!bro.terminal.persistentAvailable) {
    skipTest('persistent sessions are compiled out (no bromux)');
} else {
    assert(Array.isArray(bro.terminal.sessions({ server: SERVER })) &&
           bro.terminal.sessions({ server: SERVER }).length === 0, 'no server yet: no sessions');

    // ---- spawn persistent, fill some history, remove the element ----------------
    const a = makeTerminal();
    const sh = shell();
    a.spawn(Object.assign({ persistent: true, server: SERVER, name: 'js-test' }, sh));
    const id = a.sessionId;
    assert(typeof id === 'number' && id > 0, 'a session id: ' + id);
    assert(a.running && a.pid > 0, 'running with a pid');
    a.write((WIN ? 'for /L %i in (1,1,40) do @echo row-%i' : 'i=1; while [ $i -le 40 ]; do echo row-$i; i=$((i+1)); done') + ENTER);
    waitFor(() => /^row-40\s*$/m.test(a.screenText()), 'the rows: ' + a.screenText());
    for (let i = 0; i < 20; ++i) { advanceTime(16); wallSleep(10); }
    const screen = a.screenText();
    const history = a.scrollbackText();
    assert(/^row-1$/m.test(history), 'early rows went into history: ' + history.slice(0, 200));
    let info = session(id);
    assert(info && info.name === 'js-test' && info.running && info.clients === 1, 'listed: ' + JSON.stringify(info));

    let detached = null;
    a.addEventListener('detach', (e) => { detached = e.detail; });
    a.remove();
    waitFor(() => detached !== null, 'the detach event on removal');
    assert(detached.sessionId === id, 'detach names the session');
    assert(!a.running, 'a removed element no longer runs it');
    waitFor(() => { info = session(id); return info && info.clients === 0; }, 'the server sees no client');
    assert(info.running, 'the shell runs on after its element went');

    // ---- reattach: the same screen and history ------------------------------------
    const b = makeTerminal();
    b.attach(id, { server: SERVER });
    assert(b.running && b.sessionId === id, 'attached');
    waitFor(() => b.screenText() === screen, 'the same screen:\n' + b.screenText() + '\n-- was --\n' + screen);
    assert(b.scrollbackText() === history, 'the same scrollback');

    // ---- a second element on the same session ---------------------------------------
    const c = makeTerminal();
    c.attach(id, { server: SERVER });
    b.write('echo from-' + 'b' + ENTER);
    waitFor(() => /^from-b\s*$/m.test(c.screenText()), 'b\'s command on c');
    c.focus();
    textInput('echo from-' + 'c');
    keyDown(13);
    keyUp(13);
    waitFor(() => /^from-c\s*$/m.test(b.screenText()), 'c\'s typing on b');

    // ---- detach() and attach again on the same element ------------------------------
    b.detach();
    waitFor(() => !b.running, 'b detached');
    b.attach(id, { server: SERVER });
    waitFor(() => /^from-c\s*$/m.test(b.screenText()), 'b attached again');

    // ---- the page reloads ------------------------------------------------------------
    process.env.BRO_MUX_SERVER = SERVER;
    const exe = path.join(process.env.BRO_EXE_DIR, WIN ? 'bro-headless.exe' : 'bro-headless');
    const appDir = path.join(process.env.BRO_APP_DIR, '..', 'terminal', 'persistent_reload_app');
    const r = cp.spawnSync(exe, [appDir, path.join(appDir, 'verify.js')], { encoding: 'utf8' });
    const out = (r.stdout || '') + (r.stderr || '');
    assert(r.status === 0 && out.includes('PERSISTENT_RELOAD_OK'),
           'the session outlived the reload (child exited ' + r.status + '):\n' + out.slice(-3000));

    // ---- the program exits: every element hears it ----------------------------------
    const exits = [];
    b.addEventListener('exit', (e) => exits.push(['b', e.detail.exitCode]));
    c.addEventListener('exit', (e) => exits.push(['c', e.detail.exitCode]));
    b.write('exit 5' + ENTER);
    waitFor(() => exits.length === 2, 'both exit events');
    assert(exits.every(([, code]) => code === 5), 'exit code 5 on both: ' + JSON.stringify(exits));
    assert(!b.running && !c.running && b.exitCode === 5 && c.exitCode === 5, 'both report it');
    info = session(id);
    assert(info && !info.running && info.exitCode === 5, 'the server keeps the finished session: ' + JSON.stringify(info));
    assert(bro.terminal.closeSession(id, { server: SERVER }), 'closeSession');
    assert(!session(id), 'closed');

    // ---- kill() on a persistent element closes its session ----------------------------
    const d = makeTerminal();
    d.spawn(Object.assign({ persistent: true, server: SERVER }, sh));
    const did = d.sessionId;
    let dExit = false;
    d.addEventListener('exit', () => { dExit = true; });
    d.kill();
    waitFor(() => dExit, 'kill ends it');
    waitFor(() => !session(did), 'and removes the session');

    assert(bro.terminal.killServer({ server: SERVER }), 'killServer');
    assert(bro.terminal.sessions({ server: SERVER }).length === 0, 'no server, no sessions');
}
