// The desktop shell's permission prompts (docs/apps.md, Asking;
// docs/permissions-api.js, 'request'): one bro-headless is the shell
// (`--grant permissions`, subscribed to bro.permissions' 'request'), and app
// runs under the same scratch BRO_APP_HOME ask it instead of the dialog.
//
//   - allowed: the app is granted, and keeps the decision in the scratch store;
//   - denied: refused, and the denial is kept;
//   - the asker quits mid-prompt: the shell sees the request cancelled, and
//     nothing is kept;
//   - no shell: the app falls back to the dialog (setDialogAnswer) as before.
//
// The shell answers by capability: spawn allow, env deny, net never (it ends
// the asker with SIGTERM instead, by the pid the request carries).

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the shell and the apps');

const scratch = path.join(os.tmpdir(), 'bro_perm_prompt_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });
const home = path.join(scratch, 'home');
const stopFile = path.join(scratch, 'stop-shell');

function makeApp(name, manifest) {
    const dir = path.join(scratch, name);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'bro.json'), JSON.stringify(manifest));
    fs.writeFileSync(path.join(dir, 'index.html'), '<!DOCTYPE html><html><body>' + name + '</body></html>');
    return dir;
}

const shellApp = makeApp('shell', { id: 'org.bro.test.PromptShell', name: 'Prompt Shell' });
const askerApp = makeApp('asker', {
    id: 'org.bro.test.PromptAsker', name: 'Asker', permissions: ['spawn', 'env', 'net', 'mic'],
});

let scriptN = 0;
function writeScript(body) {
    const script = path.join(scratch, 'script' + (scriptN++) + '.js');
    fs.writeFileSync(script, body);
    return script;
}
const resultOf = (out) => {
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    return line ? JSON.parse(line.slice(line.indexOf('RESULT:') + 7)) : undefined;
};
const report = (expr) => 'console.log("RESULT:" + JSON.stringify(' + expr + '));\n';

// An app run asking for `cap` (requestPermission, the main-thread ask).
function ask(cap, pre) {
    const script = writeScript((pre || '') +
        'let got = "pending";\n' +
        'bro.app.requestPermission(' + JSON.stringify(cap) + ').then((v) => { got = v; });\n' +
        'advanceTime(16); advanceTime(16);\n' +
        report('{got, p: bro.app.permissions}'));
    const r = cp.spawnSync(headless, [askerApp, script],
        { env: { ...process.env, BRO_APP_HOME: home }, encoding: 'utf8', timeout: 60000 });
    const out = (r.stdout || '') + (r.stderr || '');
    return { status: r.status, out, result: resultOf(out) };
}
const viaShell = (out) => out.indexOf('asking the desktop shell') >= 0;
const viaDialog = (out) => out.indexOf('[permission]') >= 0;
const store = () => {
    const file = path.join(home, 'bro', 'grants.json');
    return fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
};

function pumpUntil(desc, fn, iters) {
    for (let i = 0; i < iters; i++) {
        advanceTime(16);
        wallSleep(16);
        if (fn()) return;
    }
    throw new Error('timeout waiting for ' + desc);
}

// ---- the shell ------------------------------------------------------------------
const shellScript = writeScript(
    'const fs = require("fs");\n' +
    'const seen = [];\n' +
    'bro.permissions.on("request", (req) => {\n' +
    '    seen.push({ kind: "request", id: req.id, capability: req.capability, summary: req.summary,\n' +
    '                what: req.what, app: req.app });\n' +
    '    if (req.capability === "spawn") seen.push({ kind: "respond", ok: req.respond(true) });\n' +
    '    else if (req.capability === "env") seen.push({ kind: "respond", ok: req.respond(false) });\n' +
    '    else if (req.capability === "net") process.kill(req.app.pid, "SIGTERM");\n' +
    '});\n' +
    'bro.permissions.on("cancel", (e) => {\n' +
    '    seen.push({ kind: "cancel", id: e.id, capability: e.request.capability, settled: e.request.settled,\n' +
    '                late: e.request.respond(true) });\n' +
    '});\n' +
    'console.log("SERVING:" + bro.permissions.serving);\n' +
    'for (let i = 0; i < 6000 && !fs.existsSync(' + JSON.stringify(stopFile) + '); i++) { advanceTime(16); wallSleep(16); }\n' +
    report('seen'));

let shellOut = '';
let shellExited = false;
const shell = cp.spawn(headless, ['--grant', 'permissions', shellApp, shellScript],
    { env: { ...process.env, BRO_APP_HOME: home }, stdio: 'pipe', encoding: 'utf8' });
shell.stdout.on('data', (d) => { shellOut += d; });
shell.stderr.on('data', (d) => { shellOut += d; });
shell.on('exit', () => { shellExited = true; });

let shellResult;
try {
    pumpUntil('the shell to serve prompts', () => shellOut.indexOf('SERVING:') >= 0 || shellExited, 2000);
    assert(shellOut.indexOf('SERVING:true') >= 0, 'the shell serves the prompts: ' + shellOut.slice(-2000));

    // ---- allowed: granted, and kept in the scratch store ---------------------------
    const allow = ask('spawn');
    assert(allow.result, 'the allow run ran: ' + allow.out.slice(-1500));
    assert(viaShell(allow.out) && !viaDialog(allow.out), 'the shell was asked, not the dialog: ' + allow.out.slice(-1500));
    assert(allow.result.got === true, 'the shell allowed it: ' + JSON.stringify(allow.result));
    assert(allow.result.p.granted.indexOf('spawn') >= 0, 'spawn is granted: ' + JSON.stringify(allow.result.p));
    assert(store().indexOf('"spawn":"allow"') >= 0, 'the asker kept the allow: ' + store());
    const again = ask('spawn');
    assert(again.result && again.result.got === true && !viaShell(again.out),
        'a kept allow is not asked again: ' + again.out.slice(-1500));

    // ---- denied: refused, and the denial is kept -------------------------------------
    const deny = ask('env');
    assert(deny.result && viaShell(deny.out), 'the deny run asked the shell: ' + deny.out.slice(-1500));
    assert(deny.result.got === false && deny.result.p.denied.indexOf('env') >= 0,
        'the shell denied it: ' + JSON.stringify(deny.result));
    assert(store().indexOf('"env":"deny"') >= 0, 'the asker kept the denial: ' + store());

    // ---- the asker quits mid-prompt -----------------------------------------------------
    const quit = ask('net');
    assert(viaShell(quit.out), 'the quitting run asked the shell: ' + quit.out.slice(-1500));
    assert(!quit.result || quit.result.got !== true, 'it was not granted: ' + quit.out.slice(-1500));
    assert(store().indexOf('"net"') < 0, 'nothing was kept for it: ' + store());
} finally {
    fs.writeFileSync(stopFile, 'stop');
    pumpUntil('the shell to stop', () => shellExited, 2000);
    shellResult = resultOf(shellOut);
}

assert(shellResult, 'the shell reported what it saw: ' + shellOut.slice(-2000));
const seen = shellResult;
const requests = seen.filter((e) => e.kind === 'request');
assert(JSON.stringify(requests.map((e) => e.capability)) === '["spawn","env","net"]',
    'the shell saw each first ask once, in order: ' + JSON.stringify(seen));
for (const r of requests) {
    assert(r.app.id === 'org.bro.test.PromptAsker' && r.app.name === 'Asker', 'the request names the app: ' + JSON.stringify(r));
    assert(r.app.identity.indexOf('path:') === 0 && r.app.dir.length > 0 && r.app.pid > 0,
        'with its identity, folder and pid: ' + JSON.stringify(r));
    assert(typeof r.summary === 'string' && r.summary.length > 0, 'and the capability\'s summary: ' + JSON.stringify(r));
}
assert(seen.filter((e) => e.kind === 'respond').every((e) => e.ok === true), 'each answer was delivered: ' + JSON.stringify(seen));
const cancels = seen.filter((e) => e.kind === 'cancel');
assert(cancels.length === 1 && cancels[0].capability === 'net' && cancels[0].id === requests[2].id,
    'only the quitting asker\'s request was cancelled: ' + JSON.stringify(seen));
assert(cancels[0].settled === 'cancelled' && cancels[0].late === false,
    'a cancelled request cannot be answered: ' + JSON.stringify(cancels[0]));

// ---- no shell: the dialog asks, as before ------------------------------------------------
{
    const r = ask('mic', 'setDialogAnswer(true);\n');
    assert(r.result && viaDialog(r.out) && !viaShell(r.out), 'with no shell the dialog asks: ' + r.out.slice(-1500));
    assert(r.result.got === true && r.result.p.granted.indexOf('mic') >= 0, 'and its answer stands: ' + JSON.stringify(r.result));
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
