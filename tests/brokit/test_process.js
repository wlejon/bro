// Test process global.

assert(typeof process === 'object', 'process exists');
assert(typeof process.platform === 'string', 'platform: ' + process.platform);
assert(['win32', 'linux', 'darwin', 'unknown'].indexOf(process.platform) >= 0, 'platform value');

assert(typeof process.cwd === 'function', 'cwd is fn');
const cwd = process.cwd();
assert(typeof cwd === 'string' && cwd.length > 0, 'cwd: ' + cwd);

assert(typeof process.env === 'object' && process.env !== null, 'env is object');
// PATH should exist (case-insensitive on Windows)
const path_env = process.env.PATH || process.env.Path || process.env.path;
assert(typeof path_env === 'string' && path_env.length > 0, 'PATH exists: ' + (path_env ? path_env.slice(0, 20) + '...' : 'null'));

// argv
if (Array.isArray(process.argv)) {
    assert(process.argv.length >= 1, 'argv length: ' + process.argv.length);
} else {
    console.warn('process.argv not an array');
}

// 'in' operator
assert(('PATH' in process.env) || ('Path' in process.env), 'PATH in env');

// write + delete
process.env.BROKIT_TEST_VAR = 'hello';
assert(process.env.BROKIT_TEST_VAR === 'hello', 'set env: ' + process.env.BROKIT_TEST_VAR);
delete process.env.BROKIT_TEST_VAR;
assert(process.env.BROKIT_TEST_VAR === undefined, 'deleted env: ' + process.env.BROKIT_TEST_VAR);

// missing var → undefined
assert(process.env.DEFINITELY_NOT_SET_VAR_12345 === undefined, 'missing env undefined');

// A page is a browser carrying a Node-style process: process.browser is true
// (browserify's convention, which libraries such as A-Frame read before
// treating a global `process` as Node), in a Worker too.
assert(process.browser === true, 'process.browser in the page: ' + process.browser);
{
    const url = URL.createObjectURL(new Blob(['postMessage(String(process.browser));'], { type: 'text/javascript' }));
    const w = new Worker(url);
    let got;
    w.onmessage = (e) => { got = e.data; };
    const deadline = Date.now() + 15000;
    while (got === undefined && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    w.terminate();
    assert(got === 'true', 'process.browser in a Worker: ' + got);
}
