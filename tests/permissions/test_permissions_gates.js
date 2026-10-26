// The natives each capability gates refuse without it (docs/apps.md,
// Permissions): processes (child_process, bro.app.spawn, <terminal>), the
// network (fetch, net, http's listen, WebSocket, bro.models' downloads), the
// microphone (bro.mic, getUserMedia) and bro.sys's members, each a stub
// until its own capability is granted. The same probes then run granted.
//
// Each run is a child bro-headless of an app declaring nothing, with its own
// BRO_APP_HOME (and so its own grant store); grants come from --grant.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');

const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_gates_')));
const app = path.join(scratch, 'gatesapp');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.PermGates', name: 'Gates' }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>gates</body></html>');

let scriptN = 0;
function run(body, args) {
    const script = path.join(scratch, 'script' + (scriptN++) + '.js');
    const prelude =
        'const R = {};\n' +
        'const attempt = (key, fn) => { try { const v = fn(); R[key] = "ok"; return v; } catch (e) { R[key] = String(e.message); } };\n' +
        'const settle = (key, p) => p.then(() => { R[key] = "ok"; }, (e) => { R[key] = String(e && e.message); });\n' +
        'const pump = () => { for (let i = 0; i < 40; i++) advanceTime(25); };\n';
    fs.writeFileSync(script, prelude + body + '\nconsole.log("RESULT:" + JSON.stringify(R));\n');
    const env = { ...process.env, BRO_APP_HOME: path.join(scratch, 'home' + scriptN) };
    const r = cp.spawnSync(headless, (args || []).concat([app, script]), { env, encoding: 'utf8', timeout: 90000 });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}
const refused = (msg, cap) => typeof msg === 'string' && msg !== 'ok' && msg.indexOf('(' + cap + ')') >= 0;

// ---- processes ----------------------------------------------------------------------
const spawnProbe =
    'const cpm = require("child_process");\n' +
    'attempt("spawnSync", () => { const r = cpm.spawnSync(' +
        (process.platform === 'win32' ? '"cmd", ["/c", "exit"]' : '"true", []') + '); if (r.error) throw r.error; });\n' +
    'attempt("execSync", () => cpm.execSync(' + (process.platform === 'win32' ? '"cmd /c exit"' : '"true"') + '));\n' +
    'settle("appSpawn", Promise.resolve().then(() => bro.app.spawn([])));\n' +
    'if (bro.terminal && bro.terminal.available) {\n' +
    '    const t = document.createElement("terminal"); document.body.appendChild(t); flush();\n' +
    '    attempt("terminal", () => { t.spawn({}); t.kill && t.kill(); });\n' +
    '} else R.terminal = "compiled out";\n' +
    'pump();\n';
{
    const r = run(spawnProbe);
    for (const k of ['spawnSync', 'execSync', 'appSpawn']) assert(refused(r[k], 'spawn'), k + ' refused without spawn: ' + r[k]);
    if (r.terminal !== 'compiled out') assert(refused(r.terminal, 'spawn'), '<terminal> spawn refused without spawn: ' + r.terminal);
    const g = run(spawnProbe, ['--grant', 'spawn']);
    for (const k of ['spawnSync', 'execSync', 'appSpawn']) assert(g[k] === 'ok', k + ' runs with spawn: ' + g[k]);
    if (g.terminal !== 'compiled out') assert(g.terminal === 'ok', '<terminal> spawns with spawn: ' + g.terminal);
}

// ---- network ------------------------------------------------------------------------
// Nothing listens on these: granted, each fails to connect, but not for want
// of the capability. Local URLs (file:, data:, the app's own) never need it.
const netProbe =
    'settle("fetch", fetch("http://127.0.0.1:9/x"));\n' +
    'settle("fetchData", fetch("data:text/plain,hi").then((r) => r.text()));\n' +
    'attempt("netConnect", () => { const s = require("net").connect(9, "127.0.0.1"); s.on("error", () => {}); });\n' +
    'attempt("listen", () => { const srv = require("http").createServer(() => {}); srv.listen(0, "127.0.0.1"); srv.close(); });\n' +
    'attempt("ws", () => { const w = new WebSocket("ws://127.0.0.1:9/"); w.onerror = () => {}; });\n' +
    'settle("models", bro.models.available("org.bro.test/none"));\n' +
    'pump();\n';
{
    const r = run(netProbe);
    for (const k of ['fetch', 'netConnect', 'listen', 'ws', 'models']) assert(refused(r[k], 'net'), k + ' refused without net: ' + r[k]);
    assert(r.fetchData === 'ok', 'a data: URL needs no net: ' + r.fetchData);
    const g = run(netProbe, ['--grant', 'net']);
    for (const k of ['fetch', 'netConnect', 'listen', 'ws', 'models']) {
        assert(!refused(g[k], 'net'), k + ' is not refused for net once granted: ' + g[k]);
    }
}

// ---- microphone -----------------------------------------------------------------------
const micProbe =
    'if (bro.mic && bro.mic.available !== false) attempt("mic", () => { const s = bro.mic.start({ live: false }); s && s.stop && s.stop(); });\n' +
    'else R.mic = "compiled out";\n' +
    'if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) settle("gum", navigator.mediaDevices.getUserMedia({ audio: true }));\n' +
    'else R.gum = "compiled out";\n' +
    'pump();\n';
{
    const r = run(micProbe);
    if (r.mic !== 'compiled out') assert(refused(r.mic, 'mic'), 'bro.mic.start refused without mic: ' + r.mic);
    if (r.gum !== 'compiled out') assert(refused(r.gum, 'mic'), 'getUserMedia refused without mic: ' + r.gum);
    const g = run(micProbe, ['--grant', 'mic']);
    if (g.mic !== 'compiled out') assert(!refused(g.mic, 'mic'), 'bro.mic.start not refused with mic: ' + g.mic);
    if (g.gum !== 'compiled out') assert(!refused(g.gum, 'mic'), 'getUserMedia not refused with mic: ' + g.gum);
}

// ---- bro.sys, member by member ----------------------------------------------------------
const sysProbe =
    'R.sysOut = !!(bro.sys.available === false && /compiled without/.test(String(bro.sys.reason)));\n' +
    'if (!R.sysOut) {\n' +
    '    R.power = bro.sys.power.available === false ? String(bro.sys.power.reason) : "live";\n' +
    '    R.processes = bro.sys.processes.available === false ? String(bro.sys.processes.reason) : "live";\n' +
    '    attempt("powerCall", () => bro.sys.power.getState());\n' +
    '    attempt("listProcesses", () => bro.sys.listProcesses());\n' +
    '}\n';
{
    const r = run(sysProbe, ['--grant', 'sys.power']);
    if (r.sysOut) {
        console.log('test_permissions_gates: bro.sys is compiled out of this build; its members not tested');
    } else {
        assert(r.power === 'live', 'the granted member is live: ' + r.power);
        assert(refused(r.processes, 'sys.processes'), 'an ungranted member is a stub saying why: ' + r.processes);
        assert(refused(r.listProcesses, 'sys.processes'), 'and its alias refuses: ' + r.listProcesses);
        assert(!refused(r.powerCall, 'sys.power'), 'the granted member answers: ' + r.powerCall);
    }
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
