// After the reload (persistent_reload_app/index.html): the session the first
// realm started is still running, and a <terminal> in this realm attaches to
// it and shows what was typed before the reload. Driven as
//   bro-headless tests/terminal/persistent_reload_app verify.js
// by test_terminal_persistent.js (not named test_*.js: the suite does not
// run it directly).

const server = process.env.BRO_MUX_SERVER;
const id = Number(process.env.BRO_MUX_SESSION);
assert(id > 0, 'the first run noted its session id: ' + process.env.BRO_MUX_SESSION);
assert(document.querySelectorAll('terminal').length === 0, 'a fresh document');

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

let info;
waitFor(() => {
    info = bro.terminal.sessions({ server }).find((s) => s.id === id);
    return info && info.clients === 0;
}, 'the session, detached by the reload');
assert(info.running && info.name === 'reload-test', 'running after the reload: ' + JSON.stringify(info));

const t = document.createElement('terminal');
document.body.appendChild(t);
flush();
t.attach(id, { server });
assert(t.running && t.sessionId === id, 'attached');
waitFor(() => /^before-reload\s*$/m.test(t.screenText()), 'the output from before the reload: ' + t.screenText());
bro.terminal.closeSession(id, { server });
console.log('PERSISTENT_RELOAD_OK');
