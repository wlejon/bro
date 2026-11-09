// A reload ends the children the page spawned, as a browser ends a page's
// work when it navigates away, and drops their callbacks. The page's
// ChildProcess objects poll on its timers, which the reload clears, so a child
// left running was nobody's: never reaped, and a piped one blocked once its
// pipe filled. A detached child was asked to outlive the app, so it outlives
// the page too.
//
// An inner bro-headless boots a generated app that spawns a long `sleep` (and
// a detached one) and records their pids; after the reload the attached one
// is gone and the detached one still runs. The test ends the detached one.

function main() {
    const cp = require('child_process');
    const fs = require('fs');
    const os = require('os');
    const path = require('path');

    const root = path.join(os.tmpdir(), 'bro-reload-child-' + process.pid);
    const app = path.join(root, 'app');
    fs.rmSync(root, { recursive: true, force: true });
    fs.mkdirSync(app, { recursive: true });
    fs.writeFileSync(path.join(app, 'index.html'),
        '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');
    // Spawning is a declared capability, granted for the run by --grant.
    fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({
        id: 'org.bro.test.ReloadChild', name: 'Reload Child', permissions: ['spawn'],
    }));
    fs.writeFileSync(path.join(app, 'main.js'),
        'const cp = require("child_process");\n' +
        'const boot = Number(process.env.RC_BOOT || 0) + 1;\n' +
        'process.env.RC_BOOT = String(boot);\n' +
        'if (boot === 1) {\n' +
        '  const state = { boot };\n' +
        '  const c = cp.spawn("sleep", ["30"]);\n' +
        '  c.on("exit", () => console.log("EXIT boot=" + state.boot));\n' +
        '  const d = cp.spawn("sleep", ["30"], { detached: true });\n' +
        '  process.env.RC_PID = String(c.pid);\n' +
        '  process.env.RC_DPID = String(d.pid);\n' +
        '}\n');

    // Whether a pid is still a process: kill(pid, 0) throws once it is gone (and
    // reaped; a zombie still answers).
    const alive = 'function alive(pid) { try { process.kill(pid, 0); return true; } catch (e) { return false; } }';
    const steps = [
        alive,
        'runFrames(5, { stepMs: 16 })',
        'const pid = Number(process.env.RC_PID), dpid = Number(process.env.RC_DPID)',
        'console.log("BEFORE alive=" + alive(pid))',
        'location.reload()',
        'for (let i = 0; i < 20; i++) runFrames(3, { stepMs: 16 })',
        'for (let i = 0; i < 200 && alive(pid); i++) wallSleep(10)',
        'console.log("AFTER alive=" + alive(pid) + " detached=" + alive(dpid))',
        'if (alive(dpid)) process.kill(dpid, "SIGKILL")',
        'console.log("END boot=" + process.env.RC_BOOT)',
    ].join(';\n');

    const env = Object.assign({}, process.env);
    delete env.RC_BOOT; delete env.RC_PID; delete env.RC_DPID;
    const run = cp.spawnSync(process.execPath, ['--grant', 'spawn', app, '-e', steps], { encoding: 'utf8', env });
    const out = String(run.stdout) + String(run.stderr);
    fs.rmSync(root, { recursive: true, force: true });

    assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-2000));
    assert(/BEFORE alive=true/.test(out), 'the child ran on the first page:\n' + out.slice(-1500));
    assert(/END boot=2/.test(out), 'the page reloaded:\n' + out.slice(-1500));
    assert(/AFTER alive=false detached=true/.test(out),
           'the page\'s child ended with it, the detached one did not:\n' + out.slice(-1500));
    assert(!/EXIT boot=1/.test(out), 'the old page\'s exit listener never ran:\n' + out.slice(-1500));

    console.log('reload child_process OK');
}

if (process.platform === 'win32') skipTest('uses sleep and POSIX pids');
else main();
