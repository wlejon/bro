// Managing grants from the command line (docs/apps.md, Permissions):
// `bro --permissions [<id|dir>]`, `--grant-app`, `--revoke-app`,
// `--dev-mode on|off`, and the shell's bro.permissions over the same store.
// The windowed `bro` runs these and exits without opening a window.
//
// Every run has its own BRO_APP_HOME, so the store is a scratch one.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const isWin = process.platform === 'win32';
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');
const windowed = path.join(path.dirname(headless), isWin ? 'bro.exe' : 'bro');

if (!fs.existsSync(windowed)) {
    skipTest('no windowed bro beside ' + headless + ' to run the permission commands');
} else {
    const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_cli_')));
    const appHome = path.join(scratch, 'apphome');
    const app = path.join(scratch, 'cliapp');
    fs.mkdirSync(app, { recursive: true });
    fs.writeFileSync(path.join(app, 'bro.json'),
        JSON.stringify({ id: 'org.bro.test.PermCli', name: 'Cli', permissions: ['spawn', 'net', 'seat'] }));
    fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>cli</body></html>');
    const env = { ...process.env, BRO_APP_HOME: appHome };

    const bro = (args) => {
        const r = cp.spawnSync(windowed, args, { env, encoding: 'utf8', timeout: 60000 });
        return { status: r.status, out: (r.stdout || '') + (r.stderr || '') };
    };
    const storeFile = path.join(appHome, 'bro', 'grants.json');
    const store = () => JSON.parse(fs.readFileSync(storeFile, 'utf8'));
    const decisionsOf = () => {
        const s = store();
        const key = Object.keys(s.apps).find((k) => s.apps[k].dir && path.resolve(s.apps[k].dir) === path.resolve(app)) ||
                    Object.keys(s.apps)[0];
        return key ? s.apps[key].capabilities : {};
    };

    // Grant, revoke.
    let r = bro(['--grant-app', app, 'spawn']);
    assert(r.status === 0, '--grant-app succeeds: ' + r.out);
    assert(decisionsOf().spawn === 'allow', 'the store allows spawn: ' + JSON.stringify(store()));
    r = bro(['--revoke-app', app, 'net']);
    assert(r.status === 0 && decisionsOf().net === 'deny', '--revoke-app denies net: ' + r.out + JSON.stringify(store()));

    // Shell capabilities and unknown names are refused.
    r = bro(['--grant-app', app, 'seat']);
    assert(r.status !== 0 && decisionsOf().seat === undefined, 'a shell capability cannot be granted: ' + r.out);
    r = bro(['--grant-app', app, 'nonsense']);
    assert(r.status !== 0, 'an unknown capability is refused: ' + r.out);

    // The listing names the app, its declared capabilities and decisions.
    r = bro(['--permissions', app]);
    assert(r.status === 0, '--permissions <dir> succeeds: ' + r.out);
    assert(/spawn\W+allow/.test(r.out) && /net\W+deny/.test(r.out), 'it lists the decisions: ' + r.out);
    r = bro(['--permissions']);
    assert(r.status === 0 && r.out.indexOf('org.bro.test.PermCli') >= 0, '--permissions lists every app: ' + r.out);

    // The decisions hold in a run of the app.
    const probe = (args) => {
        const script = path.join(scratch, 'probe.js');
        fs.writeFileSync(script, 'console.log("RESULT:" + JSON.stringify(bro.app.permissions));\n');
        const p = cp.spawnSync(headless, (args || []).concat([app, script]), { env, encoding: 'utf8', timeout: 90000 });
        const out = (p.stdout || '') + (p.stderr || '');
        const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
        assert(line, 'the app ran: ' + out.slice(-1500));
        return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
    };
    let p = probe();
    assert(JSON.stringify(p.granted) === '["spawn"]' && p.denied.indexOf('net') >= 0 && p.denied.indexOf('seat') >= 0,
        'the app sees the store\'s decisions: ' + JSON.stringify(p));

    // Dev mode: every declared ask capability, the denied one included; never seat.
    r = bro(['--dev-mode', 'on']);
    assert(r.status === 0 && store().devMode === true, '--dev-mode on: ' + r.out);
    p = probe();
    assert(p.granted.indexOf('net') >= 0 && p.granted.indexOf('spawn') >= 0 && p.denied.indexOf('seat') >= 0,
        'dev mode grants declared ask capabilities only: ' + JSON.stringify(p));
    r = bro(['--dev-mode', 'off']);
    assert(r.status === 0 && store().devMode === false, '--dev-mode off: ' + r.out);

    // bro.permissions, the shell's view: list, set, devMode.
    const shellScript = path.join(scratch, 'shell.js');
    fs.writeFileSync(shellScript,
        'const all = bro.permissions.list();\n' +
        'const me = all.find((e) => e.id === "org.bro.test.PermCli");\n' +
        'const caps = bro.permissions.capabilities();\n' +
        'bro.permissions.set(me.identity, "net", "allow");\n' +
        'let shellRefused = ""; try { bro.permissions.set(me.identity, "seat", "allow"); } catch (e) { shellRefused = e.message; }\n' +
        'bro.permissions.devMode = true;\n' +
        'console.log("RESULT:" + JSON.stringify({ me, caps: caps.length, shellRefused, dev: bro.permissions.devMode }));\n');
    const s = cp.spawnSync(headless, ['--grant', 'permissions', app, shellScript], { env, encoding: 'utf8', timeout: 90000 });
    const sout = (s.stdout || '') + (s.stderr || '');
    const sline = sout.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(sline, 'the shell probe ran: ' + sout.slice(-1500));
    const sr = JSON.parse(sline.slice(sline.indexOf('RESULT:') + 7));
    assert(sr.me && JSON.stringify(sr.me.declared) === '["spawn","net","seat"]', 'list() names declared capabilities: ' + JSON.stringify(sr.me));
    assert(sr.caps > 10, 'capabilities() is the catalogue');
    assert(sr.shellRefused.length > 0, 'set() refuses a shell capability');
    assert(sr.dev === true && store().devMode === true, 'devMode is set through bro.permissions');
    assert(decisionsOf().net === 'allow', 'set() wrote the store: ' + JSON.stringify(store()));

    try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
}
