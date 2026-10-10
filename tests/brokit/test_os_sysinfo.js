// require('os') system information, in Node's shapes: cpus, totalmem,
// freemem, uptime, loadavg, networkInterfaces, userInfo, release, version,
// machine (brokit's os.cpp).

const os = require('os');
const plat = os.platform();

const cpus = os.cpus();
assert(Array.isArray(cpus) && cpus.length > 0, 'cpus() lists the processors: ' + cpus.length);
assert(cpus.length === os.availableParallelism(), 'one entry per logical processor');
const c0 = cpus[0];
assert(typeof c0.model === 'string' && c0.model.length > 0, 'cpu model: ' + c0.model);
assert(typeof c0.speed === 'number' && c0.speed >= 0, 'cpu speed in MHz: ' + c0.speed);
for (const k of ['user', 'nice', 'sys', 'idle', 'irq'])
    assert(typeof c0.times[k] === 'number' && c0.times[k] >= 0, 'cpu times.' + k + ': ' + c0.times[k]);
assert(c0.times.user + c0.times.sys + c0.times.idle > 0, 'the processor has run');

const total = os.totalmem(), free = os.freemem();
assert(total > 64 * 1024 * 1024, 'totalmem in bytes: ' + total);
assert(free > 0 && free <= total, 'freemem in bytes, under totalmem: ' + free);

assert(typeof os.uptime() === 'number' && os.uptime() > 0, 'uptime in seconds: ' + os.uptime());

const load = os.loadavg();
assert(Array.isArray(load) && load.length === 3 && load.every((v) => typeof v === 'number' && v >= 0),
    'loadavg is three numbers: ' + JSON.stringify(load));
if (plat === 'win32') assert(load.join() === '0,0,0', 'loadavg is zeros on Windows, as Node');

const ifaces = os.networkInterfaces();
let loopback = false, entries = 0;
for (const name of Object.keys(ifaces)) {
    for (const e of ifaces[name]) {
        entries++;
        assert(e.family === 'IPv4' || e.family === 'IPv6', name + ' family: ' + e.family);
        assert(typeof e.address === 'string' && e.address && typeof e.netmask === 'string' && e.netmask, name + ' address/netmask');
        assert(/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/.test(e.mac), name + ' mac: ' + e.mac);
        assert(typeof e.cidr === 'string' && e.cidr.startsWith(e.address + '/'), name + ' cidr: ' + e.cidr);
        assert(typeof e.internal === 'boolean', name + ' internal');
        if (e.family === 'IPv6') assert(typeof e.scopeid === 'number', name + ' scopeid');
        if (e.internal && e.family === 'IPv4' && e.address === '127.0.0.1') {
            loopback = true;
            assert(e.netmask === '255.0.0.0', 'the loopback netmask: ' + e.netmask);
        }
    }
}
assert(entries > 0 && loopback, 'interfaces are listed, the IPv4 loopback among them');

const u = os.userInfo();
assert(typeof u.username === 'string' && u.username.length > 0, 'userInfo().username: ' + u.username);
assert(typeof u.homedir === 'string' && u.homedir.length > 0, 'userInfo().homedir');
assert(typeof u.uid === 'number' && typeof u.gid === 'number', 'userInfo uid/gid');

assert(/^\d+\.\d+/.test(os.release()), 'release is the kernel version: ' + os.release());
const ver = os.version();
assert(typeof ver === 'string' && ver.length > 0, 'version: ' + ver);
if (plat === 'win32') assert(/^Windows/.test(ver), 'version names the Windows product: ' + ver);
assert(['x86_64', 'arm64', 'aarch64', 'i686', 'arm', 'armv7l'].includes(os.machine()), 'machine: ' + os.machine());
assert(os.endianness() === 'LE', 'endianness');
assert(os.constants.signals.SIGTERM === 15 && os.constants.signals.SIGKILL === 9, 'os.constants.signals');

console.log('test_os_sysinfo: ' + cpus.length + ' x ' + c0.model + ', ' + (total / 2 ** 30).toFixed(1) +
    ' GiB, ' + ver + ' ' + os.release() + ' ' + os.machine());
console.log('test_os_sysinfo.js PASSED');
