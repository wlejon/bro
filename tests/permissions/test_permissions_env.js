// process.env and the `env` capability (docs/apps.md, Permissions): without
// it an app sees the variables that say who the user is, the locale, the
// desktop and where the standard folders are, and every other variable reads
// as unset (a token in the environment is not the app's to read); what it
// sets stays in its own view of the environment, which its child processes
// are given. With `env` it sees and changes the real environment. On Linux a
// process's environ file is env's as well, and this process's own memory is
// never an app's to read.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const isWin = process.platform === 'win32';
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');

const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_env_')));
const app = path.join(scratch, 'envapp');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.PermEnv', name: 'Env' }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>env</body></html>');

let n = 0;
function run(body, args) {
    const script = path.join(scratch, 'script' + (n++) + '.js');
    fs.writeFileSync(script,
        'const R = {};\n' +
        'const attempt = (key, fn) => { try { const v = fn(); R[key] = v === undefined ? "ok" : v; } catch (e) { R[key] = "threw: " + e.message; } };\n' +
        body + '\nconsole.log("RESULT:" + JSON.stringify(R));\n');
    const env = { ...process.env, BRO_APP_HOME: path.join(scratch, 'home' + n), SECRET_TOKEN_FOR_TEST: 'hunter2' };
    const r = cp.spawnSync(headless, (args || []).concat([app, script]), { env, encoding: 'utf8', timeout: 90000 });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}

const echoMine = isWin ? '["cmd", ["/c", "echo %MY_SET_VAR%"]]' : '["sh", ["-c", "echo $MY_SET_VAR"]]';
const probe =
    'R.secret = process.env.SECRET_TOKEN_FOR_TEST === undefined ? "unset" : process.env.SECRET_TOKEN_FOR_TEST;\n' +
    'R.inSecret = "SECRET_TOKEN_FOR_TEST" in process.env;\n' +
    'R.listed = Object.keys(process.env).indexOf("SECRET_TOKEN_FOR_TEST") >= 0;\n' +
    'R.home = !!(process.env.HOME || process.env.USERPROFILE);\n' +
    'R.listsHome = Object.keys(process.env).some((k) => k === "HOME" || k.toUpperCase() === "USERPROFILE");\n' +
    'R.broHome = !!process.env.BRO_APP_HOME;\n' +
    'process.env.MY_SET_VAR = "mine";\n' +
    'R.mine = process.env.MY_SET_VAR;\n' +
    'delete process.env.SECRET_TOKEN_FOR_TEST;\n' +
    'R.afterDelete = process.env.SECRET_TOKEN_FOR_TEST === undefined ? "unset" : process.env.SECRET_TOKEN_FOR_TEST;\n' +
    'attempt("child", () => { const [f, a] = ' + echoMine + '; const r = require("child_process").spawnSync(f, a, { encoding: "utf8" }); return String(r.stdout).trim(); });\n' +
    'if (process.platform === "linux") {\n' +
    '    attempt("environ", () => require("fs").readFileSync("/proc/self/environ").length > 0 ? "read" : "empty");\n' +
    '    attempt("mem", () => { require("fs").openSync("/proc/self/mem", "r"); return "opened"; });\n' +
    '}\n';

{
    const r = run(probe, ['--grant', 'spawn']);
    assert(r.secret === 'unset' && r.inSecret === false && r.listed === false,
           'without env a token in the environment reads as unset: ' + JSON.stringify(r));
    assert(r.home === true && r.listsHome === true, 'the home folder is always visible: ' + JSON.stringify(r));
    assert(r.broHome === true, "bro's own switches are visible: " + JSON.stringify(r));
    assert(r.mine === 'mine', 'what the app sets, it reads back: ' + r.mine);
    assert(r.child === 'mine', 'and its child processes are given it: ' + r.child);
    if (r.environ !== undefined) {
        assert(/^threw: .*\(env\)/.test(r.environ), '/proc/self/environ is env\'s: ' + r.environ);
        assert(/^threw: /.test(r.mem), '/proc/self/mem is never readable: ' + r.mem);
    }
}
{
    const r = run(probe, ['--grant', 'spawn,env,files']);
    assert(r.secret === 'hunter2' && r.inSecret === true && r.listed === true,
           'with env the whole environment is visible: ' + JSON.stringify(r));
    assert(r.afterDelete === 'unset', 'and deleting a variable deletes it: ' + r.afterDelete);
    assert(r.child === 'mine', 'a child is given what it set: ' + r.child);
    if (r.environ !== undefined) {
        assert(r.environ === 'read', 'with env /proc/self/environ reads: ' + r.environ);
        assert(/^threw: /.test(r.mem), '/proc/self/mem stays unreadable, files or not: ' + r.mem);
    }
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
