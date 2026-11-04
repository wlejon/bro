// bro.sys.processes.ioCounters(): the machine's cumulative disk and network
// counters (brosys io_counters.h), the shape an activity monitor reads, and
// two snapshots apart in time never going back. brosys's own tests cover the
// /proc parsers and each backend; this is the binding as an app sees it.
assert(typeof bro.sys === 'object', 'bro.sys namespace exists');
if (!bro.sys.available) {
    skipTest('bro.sys is unavailable in this environment');
} else {
    assert(typeof bro.sys.processes.ioCounters === 'function', 'bro.sys.processes.ioCounters is a function');
    const a = bro.sys.processes.ioCounters();
    assert(typeof a.timestamp === 'number' && a.timestamp > 0, `timestamp is a positive number (${a.timestamp})`);
    assert(Array.isArray(a.disks) && Array.isArray(a.interfaces), 'disks and interfaces are arrays');

    const nonNegative = (o, keys, what) => {
        for (const k of keys) assert(typeof o[k] === 'number' && o[k] >= 0 && Number.isFinite(o[k]), `${what}.${k} is a counter (${o[k]})`);
    };
    for (const d of a.disks) {
        assert(typeof d.name === 'string' && d.name.length > 0, 'a disk has a name');
        nonNegative(d, ['readBytes', 'writeBytes', 'reads', 'writes'], `disk ${d.name}`);
    }
    for (const n of a.interfaces) {
        assert(typeof n.name === 'string' && n.name.length > 0, 'an interface has a name');
        assert(typeof n.loopback === 'boolean', `interface ${n.name}.loopback is a boolean`);
        nonNegative(n, ['rxBytes', 'txBytes', 'rxPackets', 'txPackets'], `interface ${n.name}`);
    }
    assert(a.interfaces.length > 0, 'at least one interface (loopback, if nothing else)');
    // A Windows or macOS machine boots from a disk it has read; a Linux
    // container may expose no physical disk.
    if (require('os').platform() !== 'linux') {
        assert(a.disks.length > 0, 'at least one disk');
        assert(a.disks.some((d) => d.readBytes > 0), 'a disk has been read from');
    }

    // Read some bytes in between: the counters only move forward.
    const fs = require('fs');
    fs.readFileSync(require('path').resolve('tests/sys/test_sys_io_counters.js'));
    wallSleep(30);
    const b = bro.sys.processes.ioCounters();
    assert(b.timestamp > a.timestamp, `the timestamp moves on (${a.timestamp} -> ${b.timestamp})`);
    for (const d of b.disks) {
        const before = a.disks.find((x) => x.name === d.name);
        if (!before) continue;
        assert(d.readBytes >= before.readBytes && d.writeBytes >= before.writeBytes, `disk ${d.name} never goes back`);
        assert(d.reads >= before.reads && d.writes >= before.writes, `disk ${d.name} ops never go back`);
    }
    for (const n of b.interfaces) {
        const before = a.interfaces.find((x) => x.name === n.name);
        if (!before) continue;
        assert(n.rxBytes >= before.rxBytes && n.txBytes >= before.txBytes, `interface ${n.name} never goes back`);
    }
    console.log(`ioCounters: ${b.disks.length} disk(s), ${b.interfaces.length} interface(s)`);
}
