// Headless test for bro.sys.disks: the real UDisks2 daemon, read-only. The
// snapshot is checked against /sys/class/block and /proc/self/mountinfo, SMART
// is read from a drive that has it, and bad arguments reject; nothing is
// mounted, unmounted or ejected. Skips where there is no UDisks2 (not Linux,
// no daemon).
const fs = require('fs');
const d = bro.sys && bro.sys.disks;
assert(typeof d === 'object' && d !== null, 'bro.sys.disks exists');

function unescapeMount(p) {
    return p.replace(/\\([0-7]{3})/g, (_, o) => String.fromCharCode(parseInt(o, 8)));
}

async function main() {
    if (d.available === false) return skipTest('bro.sys.disks is unavailable: ' + d.reason);
    const st = d.getState();
    if (!st.available) return skipTest('no UDisks2 here: ' + st.reason);
    assert(Array.isArray(st.drives) && Array.isArray(st.blocks), 'getState lists drives and blocks');
    assert(st.blocks.length > 0, 'the machine has block devices');

    // Every block UDisks lists is a kernel block device.
    const kernel = new Set(fs.readdirSync('/sys/class/block'));
    for (const b of st.blocks) {
        assert(typeof b.id === 'string' && b.id.startsWith('/org/freedesktop/UDisks2/block_devices/'), 'block id: ' + b.id);
        assert(b.device.startsWith('/dev/'), 'device path: ' + b.device);
        assert(kernel.has(b.device.slice(5)), b.device + ' is in /sys/class/block');
        assert(Array.isArray(b.mountPoints) && b.mounted === (b.mountPoints.length > 0), 'mounted follows mountPoints');
        assert(typeof b.size === 'number' && b.size >= 0, 'size');
        if (b.partition) {
            assert(typeof b.partition.number === 'number' && b.partition.number > 0, 'partition number');
            assert(st.blocks.some(t => t.id === b.partition.table), 'a partition\'s table is a listed block');
        }
        if (b.drive) assert(st.drives.some(x => x.id === b.drive), 'a block\'s drive is a listed drive');
        // Lookups by id and by device path agree.
        assert(d.getBlock(b.id).device === b.device, 'getBlock by id');
        assert(d.getBlock(b.device).id === b.id, 'getBlock by device');
    }

    // Mount points match the kernel's mount table (blocks whose device the
    // table names; the daemon may lag a just-made mount, none is made here).
    const table = fs.readFileSync('/proc/self/mountinfo', 'utf8').trim().split('\n').map(l => {
        const f = l.split(' ');
        const sep = f.indexOf('-');
        return { point: unescapeMount(f[4]), source: f[sep + 2] };
    });
    for (const b of st.blocks) {
        for (const mp of b.mountPoints)
            assert(table.some(m => m.point === mp), b.device + ' mount point ' + mp + ' is in mountinfo');
    }

    for (const x of st.drives) {
        assert(typeof x.id === 'string' && x.id.startsWith('/org/freedesktop/UDisks2/drives/'), 'drive id');
        assert(typeof x.model === 'string' && typeof x.size === 'number', 'drive shape');
        assert(x.smartKind === null || x.smartKind === 'nvme' || x.smartKind === 'ata', 'smartKind');
        assert(d.getDrive(x.id).id === x.id, 'getDrive');
    }
    assert(d.getDrive('/org/freedesktop/UDisks2/drives/none') === null, 'an unknown drive is null');
    assert(d.getBlock('/dev/bro-no-such-device') === null, 'an unknown block is null');

    // Arguments are checked; an unknown device rejects, nothing is touched.
    let threw = false;
    try { await d.mount(''); } catch (e) { threw = e instanceof Error; }
    assert(threw, 'mount without a block rejects');
    threw = false;
    try { await d.unmount('/dev/bro-no-such-device'); } catch (e) { threw = /bro-no-such-device|no block/i.test(e.message); }
    assert(threw, 'unmount of an unknown device rejects, naming it');
    threw = false;
    try { await d.eject('/org/freedesktop/UDisks2/drives/none'); } catch (e) { threw = true; }
    assert(threw, 'eject of an unknown drive rejects');

    // SMART from a drive that has it.
    const smartable = st.drives.find(x => x.smartSupported);
    if (smartable) {
        const s = await d.smart(smartable.id);
        assert(s.drive === smartable.id && s.kind === smartable.smartKind, 'smart names its drive and kind');
        assert(['good', 'warning', 'failing', 'unknown'].includes(s.assessment), 'assessment: ' + s.assessment);
        assert(Array.isArray(s.attributes) && Array.isArray(s.criticalWarnings), 'smart lists');
        if (s.temperatureC !== null) assert(s.temperatureC > -20 && s.temperatureC < 120, 'temperature ' + s.temperatureC);
        if (s.kind === 'nvme') assert(typeof s.percentUsed === 'number' && typeof s.availableSpare === 'number', 'NVMe wear');
        console.log(`smart ${smartable.model}: ${s.assessment}, ${s.temperatureC} C, ${s.powerOnHours} h, ${s.percentUsed}% used`);
    } else {
        console.log('no SMART-capable drive here: smart not read');
    }

    // Events subscribe and unsubscribe.
    const onChange = () => {};
    d.on('change', onChange);
    d.off('change', onChange);
}

await main();
