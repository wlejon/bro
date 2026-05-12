// Test child_process.

const cp = require('child_process');
assert(typeof cp === 'object', 'cp require');
assert(typeof cp.execSync === 'function', 'execSync fn');

// On Windows use cmd /c
const isWin = process.platform === 'win32';
const echoCmd = isWin ? 'cmd /c echo hello' : 'echo hello';

const out = cp.execSync(echoCmd);
// May return Buffer or string; normalize
let s;
if (typeof out === 'string') s = out;
else if (out && typeof out.toString === 'function') s = out.toString();
else s = String(out);
assert(s.trim() === 'hello', 'execSync echo: "' + s.trim() + '" (type ' + (out && out.constructor && out.constructor.name) + ')');

// Non-existent command should throw
let threw = false;
try {
    cp.execSync('this_command_definitely_does_not_exist_xyz_12345');
} catch (e) {
    threw = true;
}
assert(threw, 'execSync of bad command throws');

// spawnSync
if (typeof cp.spawnSync === 'function') {
    let result;
    if (isWin) {
        result = cp.spawnSync('cmd', ['/c', 'echo', 'spawned']);
    } else {
        result = cp.spawnSync('echo', ['spawned']);
    }
    assert(typeof result === 'object', 'spawnSync returns object');
    assert('status' in result, 'spawnSync has status');
    assert('stdout' in result, 'spawnSync has stdout');
    assert('stderr' in result, 'spawnSync has stderr');
    let stdout = result.stdout;
    if (stdout && typeof stdout !== 'string') stdout = stdout.toString();
    assert(stdout && stdout.indexOf('spawned') >= 0, 'spawnSync stdout: ' + stdout);
} else {
    console.warn('spawnSync missing');
}

// execFileSync
if (typeof cp.execFileSync === 'function') {
    let out2;
    let ok = true;
    try {
        if (isWin) {
            out2 = cp.execFileSync('cmd', ['/c', 'echo', 'execfile']);
        } else {
            out2 = cp.execFileSync('echo', ['execfile']);
        }
    } catch (e) { ok = false; console.warn('execFileSync threw: ' + e.message); }
    if (ok) {
        let s2 = (typeof out2 === 'string') ? out2 : (out2 && out2.toString());
        assert(s2 && s2.indexOf('execfile') >= 0, 'execFileSync output: ' + s2);
    }
} else {
    console.warn('execFileSync missing');
}
