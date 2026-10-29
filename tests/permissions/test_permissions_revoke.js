// Grants change while the app runs (docs/apps.md, Permissions):
// a capability taken back through bro.permissions or from another process
// (`bro --revoke-app`) is revoked at once, its calls throw an Error named
// PermissionRevoked, a function kept from a gated namespace refuses too, dev
// mode turned off takes back what only it granted, a capability granted
// while running works at its next use, and each use is recorded as the
// capability's last use.
//
// Each run is a child bro-headless of an app declaring what it uses, with its
// own BRO_APP_HOME (its own grant store). The child is the host-granted shell
// for `permissions` (to change its own grants) and `spawn` (to run the CLI);
// what it is tested on comes from the store, as a user's decision would.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const isWin = process.platform === 'win32';
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');
const windowed = path.join(path.dirname(headless), isWin ? 'bro.exe' : 'bro');

const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_revoke_')));
const app = path.join(scratch, 'revokeapp');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({
    id: 'org.bro.test.PermRevoke', name: 'Revoke', permissions: ['net', 'mic', 'spawn', 'sys.power', 'clip'],
}));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>revoke</body></html>');

let runN = 0;
function run(body, opts) {
    opts = opts || {};
    const home = path.join(scratch, 'home' + (runN++));
    const env = { ...process.env, BRO_APP_HOME: home, BRO_TEST_APP: app, BRO_TEST_CLI: windowed };
    for (const cap of opts.allow || []) {
        const r = cp.spawnSync(windowed, ['--grant-app', app, cap], { env, encoding: 'utf8', timeout: 60000 });
        assert(r.status === 0, 'granting ' + cap + ' before the run: ' + (r.stdout || '') + (r.stderr || ''));
    }
    const script = path.join(scratch, 'script' + runN + '.js');
    const prelude =
        'const R = {};\n' +
        'const APP = process.env.BRO_TEST_APP;\n' +
        'const errText = (e) => (e && e.name ? e.name + ": " : "") + String(e && e.message);\n' +
        'const attempt = (key, fn) => { try { fn(); R[key] = "ok"; } catch (e) { R[key] = errText(e); } };\n' +
        'const settle = (key, p) => p.then(() => { R[key] = "ok"; }, (e) => { R[key] = errText(e); });\n' +
        'const pump = () => { for (let i = 0; i < 20; i++) advanceTime(25); };\n';
    fs.writeFileSync(script, prelude + body + '\nconsole.log("RESULT:" + JSON.stringify(R));\n');
    const args = ['--grant', opts.hostGrant || 'permissions,spawn', app, script];
    const r = cp.spawnSync(headless, args, { env, encoding: 'utf8', timeout: 120000 });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-3000));
    return { R: JSON.parse(line.slice(line.indexOf('RESULT:') + 7)), out, home };
}
const notRefused = (msg, cap) => typeof msg === 'string' && msg.indexOf('(' + cap + ')') < 0;
const revoked = (msg, cap) => typeof msg === 'string' && msg.indexOf('PermissionRevoked: ') === 0 &&
                              msg.indexOf('(' + cap + ')') >= 0;

if (!fs.existsSync(windowed)) {
    skipTest('no windowed bro beside ' + headless + ' to decide grants before the run');
} else {
    // ---- revoked through bro.permissions, in this process ---------------------------------
    {
        const { R } = run(
            'settle("fetchBefore", fetch("http://127.0.0.1:9/x"));\n' +
            'pump();\n' +
            'bro.permissions.set(APP, "net", "deny");\n' +
            'settle("fetchAfter", fetch("http://127.0.0.1:9/x"));\n' +
            'attempt("connectAfter", () => { const s = require("net").connect(9, "127.0.0.1"); s.on("error", () => {}); });\n' +
            'R.report = bro.app.permissions;\n' +
            'const entry = bro.permissions.list().find((e) => e.declared.indexOf("net") >= 0 && e.id === "org.bro.test.PermRevoke");\n' +
            'R.lastUsedNet = entry && entry.lastUsed && entry.lastUsed.net || 0;\n' +
            // Granted again while running: the next use works.
            'bro.permissions.set(APP, "net", "allow");\n' +
            'settle("fetchRegranted", fetch("http://127.0.0.1:9/x"));\n' +
            'pump();\n',
            { allow: ['net'] });
        assert(notRefused(R.fetchBefore, 'net'), 'net works while granted: ' + R.fetchBefore);
        assert(revoked(R.fetchAfter, 'net'), 'a fetch after the revocation is a PermissionRevoked: ' + R.fetchAfter);
        assert(revoked(R.connectAfter, 'net'), 'and so is a socket: ' + R.connectAfter);
        assert(R.report.denied.indexOf('net') >= 0, 'bro.app.permissions reports it denied: ' + JSON.stringify(R.report));
        assert(R.lastUsedNet > Date.now() - 600000, 'the use was recorded as net\'s last: ' + R.lastUsedNet);
        assert(notRefused(R.fetchRegranted, 'net'), 'granted again, the next fetch is not refused: ' + R.fetchRegranted);
    }

    // ---- a gated namespace, and a function kept from it -----------------------------------
    {
        const { R } = run(
            'R.sysOut = !!(bro.sys.available === false && /compiled without/.test(String(bro.sys.reason)));\n' +
            'if (!R.sysOut) {\n' +
            '    R.before = bro.sys.power.available;\n' +
            '    const kept = bro.sys.power.getState;\n' +
            '    attempt("keptBefore", () => kept());\n' +
            '    bro.permissions.set(APP, "sys.power", "deny");\n' +
            '    R.after = bro.sys.power.available;\n' +
            '    R.reason = String(bro.sys.power.reason);\n' +
            '    attempt("callAfter", () => bro.sys.power.getState());\n' +
            '    attempt("keptAfter", () => kept());\n' +
            '}\n',
            { allow: ['sys.power'] });
        if (R.sysOut) {
            console.log('test_permissions_revoke: bro.sys is compiled out of this build; namespace revocation not tested');
        } else {
            assert(R.before === true, 'the granted namespace is live: ' + R.before);
            assert(notRefused(R.keptBefore, 'sys.power'), 'its function answers: ' + R.keptBefore);
            assert(R.after === false && R.reason.indexOf('(sys.power)') >= 0, 'revoked, it is the stub: ' + R.after + ' ' + R.reason);
            assert(revoked(R.callAfter, 'sys.power'), 'a call through it is a PermissionRevoked: ' + R.callAfter);
            assert(revoked(R.keptAfter, 'sys.power'), 'and so is a function kept from before: ' + R.keptAfter);
        }
    }

    // ---- revoked from another process ------------------------------------------------------
    {
        const { R, out } = run(
            'settle("fetchBefore", fetch("http://127.0.0.1:9/x"));\n' +
            'pump();\n' +
            'const r = require("child_process").spawnSync(process.env.BRO_TEST_CLI, ["--revoke-app", APP, "net"], { encoding: "utf8" });\n' +
            'R.cli = r.status;\n' +
            'R.cliOut = String(r.stdout) + String(r.stderr);\n' +
            // The store is read again within a quarter second of wall time.
            // The store is polled on wall time: each turn waits 50 ms of it
            // (sleep() is virtual) and runs a frame.
            'let waited = 0;\n' +
            'while (bro.app.permissions.denied.indexOf("net") < 0 && waited < 5000) { wallSleep(50); advanceTime(16); waited += 50; }\n' +
            'R.waited = waited;\n' +
            'settle("fetchAfter", fetch("http://127.0.0.1:9/x"));\n' +
            'pump();\n',
            { allow: ['net'] });
        assert(R.cli === 0, 'bro --revoke-app ran: ' + R.cli);
        assert(R.waited < 5000, 'the running app saw the revocation, after ' + R.waited + ' ms; the CLI said: ' +
               R.cliOut + '; the app logged: ' + out.split(/\r?\n/).filter((l) => /permissions|grants/.test(l)).join(' | '));
        assert(revoked(R.fetchAfter, 'net'), 'its next fetch is a PermissionRevoked: ' + R.fetchAfter);
    }

    // ---- what the app holds open closes with the grant ------------------------------------
    {
        const longRun = isWin ? '{ command: "cmd.exe", args: ["/c", "ping -n 30 127.0.0.1 >nul"] }'
                              : '{ command: "sleep", args: ["30"] }';
        const { R } = run(
            'const srv = require("net").createServer(() => {});\n' +
            'srv.on("error", (e) => { R.serverErr = errText(e); });\n' +
            'srv.on("close", () => { R.serverClosed = true; });\n' +
            'attempt("listen", () => srv.listen(0, "127.0.0.1"));\n' +
            'const child = require("child_process").spawn(' + (isWin ? '"cmd", ["/c", "ping -n 30 127.0.0.1 >nul"]' : '"sleep", ["30"]') + ');\n' +
            'child.on("error", (e) => { R.childErr = errText(e); });\n' +
            'child.on("exit", () => { R.childExit = true; });\n' +
            'const term = document.createElement("terminal");\n' +
            'R.termOut = typeof term.spawn !== "function";\n' +
            'if (!R.termOut) {\n' +
            '    document.body.appendChild(term);\n' +
            '    term.addEventListener("permissionrevoked", (e) => { R.termEvent = e.detail && e.detail.message; });\n' +
            '    term.addEventListener("exit", () => { R.termExit = true; });\n' +
            '    attempt("termSpawn", () => term.spawn(' + longRun + '));\n' +
            '}\n' +
            'R.micOut = !(bro.mic && typeof bro.mic.start === "function");\n' +
            'if (!R.micOut) {\n' +
            '    attempt("micStart", () => bro.mic.start({ live: false, onError: (e) => { R.micErr = errText(e); } }));\n' +
            '    R.micBefore = bro.mic.isActive();\n' +
            '}\n' +
            'pump();\n' +
            'R.termRunningBefore = !R.termOut && term.running;\n' +
            'bro.permissions.set(APP, "net", "deny");\n' +
            'bro.permissions.set(APP, "spawn", "deny");\n' +
            'bro.permissions.set(APP, "mic", "deny");\n' +
            'for (let i = 0; i < 200 && !(R.childExit && (R.termOut || R.termExit)); i++) { wallSleep(25); advanceTime(25); }\n' +
            'R.micAfter = !R.micOut && bro.mic.isActive();\n',
            { allow: ['net', 'spawn', 'mic'], hostGrant: 'permissions' });
        assert(R.listen === 'ok', 'the server listened: ' + R.listen);
        assert(revoked(R.serverErr, 'net'), 'the listening server errors with a PermissionRevoked: ' + R.serverErr);
        assert(R.serverClosed === true, 'and closes');
        assert(revoked(R.childErr, 'spawn'), 'a running child process errors with a PermissionRevoked: ' + R.childErr);
        assert(R.childExit === true, 'and is ended');
        if (R.termOut) {
            console.log('test_permissions_revoke: <terminal> is compiled out of this build; not tested');
        } else {
            assert(R.termSpawn === 'ok' && R.termRunningBefore === true, 'the terminal ran its program: ' + R.termSpawn);
            assert(typeof R.termEvent === 'string' && R.termEvent.indexOf('(spawn)') >= 0,
                   'the terminal says why in a permissionrevoked event: ' + R.termEvent);
            assert(R.termExit === true, 'and its program is ended');
        }
        if (R.micOut) {
            console.log('test_permissions_revoke: bro.mic is compiled out of this build; not tested');
        } else {
            assert(R.micStart === 'ok' && R.micBefore === true, 'bro.mic ran: ' + R.micStart);
            assert(revoked(R.micErr, 'mic'), 'bro.mic stops with a PermissionRevoked: ' + R.micErr);
            assert(R.micAfter === false, 'and is no longer active');
        }
    }

    // ---- dev mode off takes back what only it granted --------------------------------------
    {
        const { R } = run(
            'bro.permissions.devMode = true;\n' +
            'R.on = bro.app.permissions.granted.indexOf("clip") >= 0;\n' +
            'R.clipOn = bro.clip.available;\n' +
            'bro.permissions.devMode = false;\n' +
            'R.off = bro.app.permissions.granted.indexOf("clip") >= 0;\n' +
            'R.clipOff = bro.clip.available;\n');
        assert(R.on === true, 'dev mode grants a declared capability while the app runs');
        assert(R.off === false, 'and turning it off takes it back: ' + JSON.stringify(R));
        if (R.clipOn !== false || R.clipOff !== false) {
            // bro.clip compiled in: the namespace follows.
            assert(R.clipOn === true && R.clipOff === false, 'bro.clip follows dev mode: ' + R.clipOn + ' ' + R.clipOff);
        }
    }
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
