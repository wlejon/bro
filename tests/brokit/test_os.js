// Test os module.

const os = require('os');
assert(typeof os === 'object', 'os require');

assert(typeof os.platform === 'function', 'platform fn');
const plat = os.platform();
assert(typeof plat === 'string', 'platform string: ' + plat);
assert(['win32', 'linux', 'darwin'].indexOf(plat) >= 0, 'platform value: ' + plat);

assert(typeof os.arch === 'function', 'arch fn');
const arch = os.arch();
assert(typeof arch === 'string', 'arch string: ' + arch);

assert(typeof os.homedir === 'function', 'homedir fn');
const home = os.homedir();
assert(typeof home === 'string' && home.length > 0, 'homedir nonempty: ' + home);

assert(typeof os.tmpdir === 'function', 'tmpdir fn');
const tmp = os.tmpdir();
assert(typeof tmp === 'string' && tmp.length > 0, 'tmpdir nonempty: ' + tmp);

assert(typeof os.EOL === 'string', 'EOL string: ' + JSON.stringify(os.EOL));
if (plat === 'win32') {
    assert(os.EOL === '\r\n', 'win EOL: ' + JSON.stringify(os.EOL));
}

// hostname
if (typeof os.hostname === 'function') {
    const hn = os.hostname();
    assert(typeof hn === 'string', 'hostname: ' + hn);
} else {
    console.warn('os.hostname missing'); // BUG: os.hostname-missing
}

// cpus (mentioned in user spec, not in docs)
if (typeof os.cpus === 'function') {
    const c = os.cpus();
    assert(Array.isArray(c), 'cpus is array');
    assert(c.length > 0, 'cpus length > 0: ' + c.length);
} else {
    console.warn('os.cpus missing'); // BUG: os.cpus-missing
}
