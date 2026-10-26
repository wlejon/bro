// Capabilities granted per app (docs/apps.md, Permissions): what is asked,
// what is never asked, what the user's answer does on the next run, dev mode,
// the capabilities only a host grants, bro.app.permissions and
// requestPermission, and a manifest naming no capability.
//
// Each case runs a fixture app in a child bro-headless with its own
// BRO_APP_HOME, which also holds bro's grant store, so the user's own grants
// are never read or written.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');

const scratch = path.join(os.tmpdir(), 'bro_perm_ask_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });

function makeApp(name, manifest) {
    const dir = path.join(scratch, name);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'bro.json'), JSON.stringify(manifest));
    fs.writeFileSync(path.join(dir, 'index.html'), '<!DOCTYPE html><html><body>' + name + '</body></html>');
    return dir;
}

// Runs `body` (a script whose last act is to print RESULT:<json>) in the app;
// answers {status, out, result}. `home` is the run's BRO_APP_HOME.
let scriptN = 0;
function run(appDir, home, body, args) {
    const script = path.join(scratch, 'script' + (scriptN++) + '.js');
    fs.writeFileSync(script, body);
    const env = { ...process.env, BRO_APP_HOME: home };
    const r = cp.spawnSync(headless, (args || []).concat([appDir, script]), { env, encoding: 'utf8', timeout: 90000 });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    return { status: r.status, out, result: line ? JSON.parse(line.slice(line.indexOf('RESULT:') + 7)) : undefined };
}
const report = (expr) => 'console.log("RESULT:" + JSON.stringify(' + expr + '));\n';
const asked = (out) => out.indexOf('[permission]') >= 0;

// A probe of what a spawn attempt does: '' when it ran, else the error.
const trySpawn = '(() => { try { require("child_process").spawnSync(' +
    (process.platform === 'win32' ? '"cmd", ["/c", "exit"]' : '"true", []') + '); return ""; } ' +
    'catch (e) { return String(e.message); } })()';

// ---- undeclared: denied, nobody asked -----------------------------------------
{
    const app = makeApp('undeclared', { id: 'org.bro.test.PermUndeclared', name: 'Undeclared' });
    const home = path.join(scratch, 'home-undeclared');
    const r = run(app, home, 'setDialogAnswer(true);\n' + report('{spawn: ' + trySpawn + ', p: bro.app.permissions}'));
    assert(r.result, 'the undeclared probe ran: ' + r.out.slice(-1500));
    assert(/spawn/.test(r.result.spawn) && /not declare|declare/.test(r.result.spawn),
        'an undeclared capability is refused, naming it: ' + r.result.spawn);
    assert(!asked(r.out), 'and nobody was asked, though the script would have said yes: ' + r.out.slice(-1500));
    assert(r.result.p.declared.length === 0 && r.result.p.granted.length === 0, 'nothing declared or granted');
}

// ---- declared: asked once, the answer persists --------------------------------
{
    const app = makeApp('declared', { id: 'org.bro.test.PermDeclared', name: 'Declared', permissions: ['spawn'] });
    const home = path.join(scratch, 'home-declared');

    // Unanswered (headless with no answer chosen): asked, refused, not kept.
    const r0 = run(app, home, report('{spawn: ' + trySpawn + ', p: bro.app.permissions}'));
    assert(r0.result && asked(r0.out), 'a declared capability is asked for at first use: ' + r0.out.slice(-1500));
    assert(r0.result.spawn.length > 0, 'an unanswered ask refuses: ' + r0.result.spawn);
    assert(JSON.stringify(r0.result.p.undecided) === '["spawn"]', 'and decides nothing: ' + JSON.stringify(r0.result.p));

    // Denied: refused, and the next run is refused without asking.
    const r1 = run(app, home, 'setDialogAnswer(false);\n' + report('{spawn: ' + trySpawn + '}'));
    assert(r1.result && asked(r1.out) && r1.result.spawn.length > 0, 'the user said no: ' + r1.out.slice(-1500));
    const r2 = run(app, home, 'setDialogAnswer(true);\n' + report('{spawn: ' + trySpawn + ', p: bro.app.permissions}'));
    assert(r2.result && !asked(r2.out), 'a denial is kept: not asked again: ' + r2.out.slice(-1500));
    assert(r2.result.spawn.length > 0 && JSON.stringify(r2.result.p.denied) === '["spawn"]',
        'and refused: ' + JSON.stringify(r2.result));

    // Allowed, in another store: used, and the next run uses it without asking.
    const home2 = path.join(scratch, 'home-declared-allow');
    const r3 = run(app, home2, 'setDialogAnswer(true);\n' + report('{spawn: ' + trySpawn + '}'));
    assert(r3.result && asked(r3.out) && r3.result.spawn === '', 'the user said yes: ' + r3.out.slice(-1500));
    const r4 = run(app, home2, 'setDialogAnswer(false);\n' + report('{spawn: ' + trySpawn + ', p: bro.app.permissions}'));
    assert(r4.result && !asked(r4.out) && r4.result.spawn === '', 'an allow is kept: ' + r4.out.slice(-1500));
    assert(JSON.stringify(r4.result.p.granted) === '["spawn"]', 'granted: ' + JSON.stringify(r4.result.p));
    const store = JSON.parse(fs.readFileSync(path.join(home2, 'bro', 'grants.json'), 'utf8'));
    assert(JSON.stringify(store).indexOf('"spawn":"allow"') >= 0, 'grants.json holds the decision: ' + JSON.stringify(store));
}

// ---- requestPermission ---------------------------------------------------------
{
    const app = makeApp('request', { id: 'org.bro.test.PermRequest', name: 'Request', permissions: ['net', 'shell'] });
    const home = path.join(scratch, 'home-request');
    const body = 'setDialogAnswer(true);\n' +
        'let out = {};\n' +
        'bro.app.requestPermission("net").then((v) => { out.net = v; });\n' +
        'bro.app.requestPermission("spawn").then((v) => { out.spawn = v; });\n' +
        'bro.app.requestPermission("shell").then((v) => { out.shell = v; });\n' +
        'bro.app.requestPermission("nonsense").then(() => { out.bad = "resolved"; }, (e) => { out.bad = String(e.message); });\n' +
        'advanceTime(16); advanceTime(16);\n' +
        report('{out, p: bro.app.permissions}');
    const r = run(app, home, body);
    assert(r.result, 'the request probe ran: ' + r.out.slice(-1500));
    const o = r.result.out;
    assert(o.net === true, 'a declared capability the user allows resolves true: ' + JSON.stringify(o));
    assert(o.spawn === false, 'an undeclared one resolves false: ' + JSON.stringify(o));
    assert(o.shell === false, 'a shell capability resolves false: ' + JSON.stringify(o));
    assert(/not a capability|capabilit/.test(o.bad), 'an unknown name rejects: ' + o.bad);
    const p = r.result.p;
    assert(JSON.stringify(p.declared) === '["net","shell"]', 'declared: ' + JSON.stringify(p));
    assert(JSON.stringify(p.granted) === '["net"]' && JSON.stringify(p.denied) === '["shell"]' && p.undecided.length === 0,
        'granted / denied / undecided: ' + JSON.stringify(p));
}

// ---- dev mode: declared ask capabilities, never shell ---------------------------
{
    const app = makeApp('devmode', { id: 'org.bro.test.PermDev', name: 'Dev', permissions: ['spawn', 'compositor', 'shell'] });
    const home = path.join(scratch, 'home-dev');
    // A store, edited by hand, that denies spawn and would allow the shell
    // capabilities: dev mode overrides the denial, and nothing gives a shell
    // capability. (`bro --grant-app` refuses to write those; see
    // test_permissions_cli.js.)
    fs.mkdirSync(path.join(home, 'bro'), { recursive: true });
    fs.writeFileSync(path.join(home, 'bro', 'grants.json'), JSON.stringify({
        devMode: true,
        // The identity: the canonical folder, with forward slashes.
        apps: { ['path:' + fs.realpathSync(app).replace(/\\/g, '/')]:
                { capabilities: { spawn: 'deny', compositor: 'allow', shell: 'allow' } } },
    }));
    const r = run(app, home, report('{spawn: ' + trySpawn + ', p: bro.app.permissions, comp: bro.compositor.available}'));
    assert(r.result, 'the dev-mode probe ran: ' + r.out.slice(-1500));
    assert(r.result.spawn === '', 'dev mode grants a declared ask capability: ' + r.result.spawn);
    assert(JSON.stringify(r.result.p.granted) === '["spawn"]', 'only that: ' + JSON.stringify(r.result.p));
    assert(r.result.p.denied.indexOf('shell') >= 0 && r.result.p.denied.indexOf('compositor') >= 0,
        'shell capabilities stay denied: ' + JSON.stringify(r.result.p));
    assert(r.result.comp === false, 'bro.compositor stays a stub');
    assert(/DEV MODE/i.test(r.out), 'dev mode is logged loudly');

    // The same store with dev mode off: its denial stands.
    const store = JSON.parse(fs.readFileSync(path.join(home, 'bro', 'grants.json'), 'utf8'));
    store.devMode = false;
    fs.writeFileSync(path.join(home, 'bro', 'grants.json'), JSON.stringify(store));
    const off = run(app, home, report('{spawn: ' + trySpawn + ', p: bro.app.permissions}'));
    assert(off.result && off.result.spawn.length > 0 && off.result.p.denied.indexOf('spawn') >= 0,
        'without dev mode the stored denial refuses spawn: ' + JSON.stringify(off.result));
}

// ---- shell capabilities: only a host grants them ---------------------------------
{
    const app = makeApp('shellcap', { id: 'org.bro.test.PermShell', name: 'Shell', permissions: ['permissions'] });
    const home = path.join(scratch, 'home-shell');
    const probe = report('{p: bro.app.permissions, perms: bro.permissions.available}');
    const r0 = run(app, home, 'setDialogAnswer(true);\n' + probe);
    assert(r0.result && !asked(r0.out), 'a shell capability is never asked for: ' + r0.out.slice(-1500));
    assert(r0.result.perms === false && JSON.stringify(r0.result.p.denied) === '["permissions"]',
        'and is denied: ' + JSON.stringify(r0.result));
    // The host grants it.
    const r1 = run(app, home, probe, ['--grant', 'permissions']);
    assert(r1.result && r1.result.perms === true, 'a host grant gives it: ' + r1.out.slice(-1500));
    assert(/grant/i.test(r1.out), 'a host grant is logged');
}

// ---- the manifest names no capability ---------------------------------------------
{
    const app = makeApp('unknown', { id: 'org.bro.test.PermUnknown', name: 'Unknown', permissions: ['spawn', 'vfs'] });
    const r = run(app, path.join(scratch, 'home-unknown'), report('"ran"'));
    assert(r.status !== 0 && r.result === undefined, 'an app naming an unknown permission does not run: ' + r.out.slice(-1500));
    assert(/"vfs"/.test(r.out) && /not a capability/.test(r.out), 'and says which name is wrong: ' + r.out.slice(-1500));
}

// ---- the id `bro` is bro's own ------------------------------------------------------
{
    const app = makeApp('reserved', { id: 'bro', name: 'Reserved' });
    const r = run(app, path.join(scratch, 'home-reserved'), report('bro.app.id'));
    assert(r.result !== 'bro', 'no app runs as bro: ' + r.out.slice(-1500));
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
