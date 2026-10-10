// process.kill(pid, signal), as Node has it: signal 0 asks whether the
// process exists; Windows ends a process for SIGINT/SIGQUIT/SIGTERM/SIGKILL
// with TerminateProcess; a failure throws an Error with code/errno/syscall.

const cp = require('child_process');
const isWin = process.platform === 'win32';

assert(typeof process.kill === 'function', 'process.kill exists');
assert(process.kill(process.pid, 0) === true, 'signal 0 on this process is true');

const codeOf = (fn) => { try { fn(); } catch (e) { return e && (e.code || e.name); } return null; };
const sleeper = () => isWin ? cp.spawn('ping', ['-n', '30', '127.0.0.1'], { stdio: 'ignore' })
                            : cp.spawn('sleep', ['30'], { stdio: 'ignore' });
const waitExit = (child) => {
    let got = null;
    child.on('exit', (code, signal) => { got = { code, signal }; });
    const end = perf.now() + 10000;
    while (!got && perf.now() < end) { advanceTime(16); wallSleep(10); }
    return got;
};

const a = sleeper();
const until = perf.now() + 5000;
while (!(a.pid > 0) && perf.now() < until) { advanceTime(16); wallSleep(5); }
assert(a.pid > 0, 'a child to signal: ' + a.pid);
assert(process.kill(a.pid, 0) === true, 'signal 0: the child exists');
assert(process.kill(a.pid) === true, 'the default signal (SIGTERM) is sent');
const ea = waitExit(a);
assert(ea, 'SIGTERM ended the child');
if (!isWin) assert(ea.signal === 'SIGTERM', 'it ended on SIGTERM: ' + JSON.stringify(ea));
assert(codeOf(() => process.kill(a.pid, 0)) === 'ESRCH', 'signal 0 on an exited child: ESRCH');

const b = sleeper();
const until2 = perf.now() + 5000;
while (!(b.pid > 0) && perf.now() < until2) { advanceTime(16); wallSleep(5); }
assert(process.kill(b.pid, 'SIGKILL') === true, 'SIGKILL by name');
assert(waitExit(b), 'SIGKILL ended the child');

let err = null;
try { process.kill(b.pid, 15); } catch (e) { err = e; }
assert(err instanceof Error && err.code === 'ESRCH' && err.syscall === 'kill' && err.errno < 0,
    'a failed kill throws Node\'s error: ' + (err && JSON.stringify({ code: err.code, syscall: err.syscall, errno: err.errno })));

assert(codeOf(() => process.kill('123')) === 'TypeError', 'a pid that is not a number');
assert(codeOf(() => process.kill(process.pid, 'SIGNOPE')) === 'TypeError', 'an unknown signal name');
if (isWin) assert(codeOf(() => process.kill(process.pid, 'SIGHUP')) === 'ENOSYS', 'Windows: SIGHUP is ENOSYS');
console.log('test_process_kill.js PASSED');
