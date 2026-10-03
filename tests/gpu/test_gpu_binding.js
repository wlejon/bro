// Binding tests for bro.gpu — the always-present runtime GPU-backend probe.
// bro.gpu never loads weights, so the whole surface is testable here:
// available/backend/devices shape, memoryInfo/deviceName/trim contracts, and
// cross-property consistency. When built without BRO_WITH_TENSOR the namespace
// is a static stub, but it is deliberately behaviorally identical to a real
// CPU-only binding (available:false, backend:'cpu', devices:['cpu'], probe
// methods returning null/null/false) — so one contract covers both builds.

assert(typeof bro === 'object', 'bro global exists');
assert(bro.gpu !== undefined && bro.gpu !== null, 'bro.gpu namespace exists');

const KNOWN = ['hip', 'cuda', 'metal', 'vulkan', 'cpu'];

{
    assert(typeof bro.gpu.available === 'boolean', 'available is a boolean');
    assert(typeof bro.gpu.backend === 'string', 'backend is a string');
    assert(KNOWN.includes(bro.gpu.backend),
           'backend is one of hip/cuda/metal/vulkan/cpu: ' + bro.gpu.backend);

    const devices = bro.gpu.devices;
    assert(Array.isArray(devices), 'devices is an array');
    assert(devices.includes('cpu'), 'devices always includes cpu');
    for (const d of devices) {
        assert(KNOWN.includes(d), 'device name is one of hip/cuda/metal/vulkan/cpu: ' + d);
    }
    assert(devices.includes(bro.gpu.backend), 'default backend is a registered device');

    // Every registered backend was compiled in (the build's answer bounds the
    // driver's); with HIP and Vulkan both registered, Vulkan is the default
    // unless the run asked for HIP (BROTENSOR_PREFER_HIP=1 or
    // BROTENSOR_DEFAULT_DEVICE=hip; the env is invisible here, so only the
    // two-way consistency is checked).
    const compiled = bro.gpu.compiledBackends;
    assert(Array.isArray(compiled) && compiled.includes('cpu'), 'compiledBackends includes cpu');
    for (const d of devices) {
        assert(compiled.includes(d), 'registered backend ' + d + ' is in compiledBackends');
    }
    if (devices.includes('hip') && devices.includes('vulkan')) {
        assert(bro.gpu.backend === 'vulkan' || bro.gpu.backend === 'hip',
               'an AMD build with both backends defaults to one of them: ' + bro.gpu.backend);
    }
    // 'vk' is an alias of 'vulkan'.
    assert(bro.gpu.deviceCount('vk') === bro.gpu.deviceCount('vulkan'), "deviceCount('vk') matches 'vulkan'");

    // available means "the default device is a GPU" — consistent with backend.
    assert(bro.gpu.available === (bro.gpu.backend !== 'cpu'),
           'available is consistent with backend (' + bro.gpu.backend + ')');

    // Getters are stable across reads (driver probe is cached).
    assert(bro.gpu.backend === bro.gpu.backend, 'backend getter is stable');

    // deviceCount: cards per backend. 1 for cpu, >=1 for a registered GPU
    // backend (multi-GPU reports the card count), 0 for an unregistered one.
    assert(bro.gpu.deviceCount('cpu') === 1, 'deviceCount(cpu) is 1');
    for (const b of KNOWN) {
        const n = bro.gpu.deviceCount(b);
        assert(Number.isInteger(n) && n >= 0, 'deviceCount is a non-negative int: ' + b);
        assert(devices.includes(b) === (n > 0),
               'deviceCount(' + b + ')=' + n + ' agrees with devices');
    }

    for (const f of ['memoryInfo', 'deviceName', 'trim', 'deviceCount']) {
        assert(typeof bro.gpu[f] === 'function', 'bro.gpu.' + f + ' is a function');
    }

    // memoryInfo: {freeBytes, totalBytes} or null; always null for cpu.
    assert(bro.gpu.memoryInfo('cpu') === null, 'memoryInfo(cpu) is null');
    const mem = bro.gpu.memoryInfo();
    if (bro.gpu.available) {
        assert(mem !== null, 'memoryInfo() reports on the default GPU');
        assert(typeof mem.freeBytes === 'number' && typeof mem.totalBytes === 'number',
               'memoryInfo() has numeric freeBytes/totalBytes');
        assert(mem.totalBytes > 0 && mem.freeBytes >= 0 && mem.freeBytes <= mem.totalBytes,
               'memoryInfo() bytes are sane: free=' + mem.freeBytes +
               ' total=' + mem.totalBytes);
    } else {
        assert(mem === null, 'memoryInfo() is null on a CPU-default build');
    }

    // deviceName: human-readable string or null; always null for cpu.
    assert(bro.gpu.deviceName('cpu') === null, 'deviceName(cpu) is null');
    const name = bro.gpu.deviceName();
    if (bro.gpu.available) {
        assert(typeof name === 'string' && name.length > 0,
               'deviceName() is a non-empty string: ' + name);
    } else {
        assert(name === null, 'deviceName() is null on a CPU-default build');
    }

    // trim: boolean; cpu has no trimmable allocator.
    assert(typeof bro.gpu.trim() === 'boolean', 'trim() returns a boolean');
    assert(bro.gpu.trim('cpu') === false, 'trim(cpu) is false');

    // 'cuda:N' addresses one card of a multi-GPU box; index 0 is the same
    // device as the bare backend name, and an out-of-range index reports
    // nothing rather than throwing.
    if (bro.gpu.backend !== 'cpu') {
        const b = bro.gpu.backend;
        assert(bro.gpu.deviceName(b + ':0') === bro.gpu.deviceName(b),
               b + ':0 names the same card as ' + b);
        const past = bro.gpu.deviceCount(b);
        assert(bro.gpu.memoryInfo(b + ':' + past) === null,
               'memoryInfo past the last card is null');
    }

    // A non-string device arg falls back to the default device, never throws.
    let threw = false;
    try { bro.gpu.memoryInfo(42); } catch (e) { threw = true; }
    assert(!threw, 'memoryInfo(non-string) does not throw');

    console.log('bro.gpu probe contract OK (backend=' + bro.gpu.backend +
                ', devices=' + JSON.stringify(devices) + ')');
}
