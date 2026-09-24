// A spawned child ends with the app that started it, however the app exits;
// `detached: true` opts out. Without this, Windows leaves children running
// after bro exits, and a child looping on a dead stdout pipe runs forever
// (broworkshop's procwatch left dozens of PowerShell / typeperf processes
// behind, one set per test run).
//
// An inner bro-headless (process.execPath) spawns a long-lived child and a
// detached one, prints their pids and exits; then this test checks which of
// them are still running.

const cp = require('child_process');
const isWin = process.platform === 'win32';

if (!isWin && process.platform !== 'linux') {
    console.log('child lifetime: no parent-death hook on ' + process.platform + '; skipping');
} else {
    assert(typeof process.execPath === 'string' && process.execPath !== 'bro',
           'process.execPath names the executable: ' + process.execPath);
    assert(typeof process.pid === 'number' && process.pid > 1, 'process.pid is the real pid: ' + process.pid);

    const longCmd = isWin ? "'ping', ['-n', '60', '127.0.0.1']" : "'sleep', ['60']";
    const inner =
        "const cp = require('child_process');" +
        "const a = cp.spawn(" + longCmd + ", { stdio: 'pipe' });" +
        "const b = cp.spawn(" + longCmd + ", { detached: true });" +
        "console.log('PIDS ' + a.pid + ' ' + b.pid);";
    const out = String(cp.execFileSync(process.execPath, [bro.appDir, '-e', inner], { encoding: 'utf8' }));
    const m = /PIDS (\d+) (\d+)/.exec(out);
    assert(m, 'inner run printed the child pids: ' + out.slice(-300));
    const child = Number(m[1]), detached = Number(m[2]);

    const alive = (pid) => {
        if (isWin) {
            const r = String(cp.execSync('tasklist /FI "PID eq ' + pid + '" /NH', { encoding: 'utf8' }));
            return new RegExp('\\s' + pid + '\\s').test(r);
        }
        try { cp.execSync('kill -0 ' + pid); return true; } catch { return false; }
    };

    // Exit-time teardown is the OS's; give it up to 5 s of wall time.
    const t0 = Date.now();
    while (alive(child) && Date.now() - t0 < 5000) cp.execSync(isWin ? 'ping -n 2 127.0.0.1 >NUL' : 'sleep 0.2');
    assert(!alive(child), 'the child ended with the app that spawned it (pid ' + child + ')');
    assert(alive(detached), 'a detached child outlives the app (pid ' + detached + ')');
    if (isWin) cp.execSync('taskkill /PID ' + detached + ' /F');
    else cp.execSync('kill -9 ' + detached);
    console.log('child lifetime OK');
}
