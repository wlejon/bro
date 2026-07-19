// Worker side of test_postmessage_types.
//
// Describe what each value deserialized as, then echo the whole payload back
// so the main thread can check the return leg through the same serializer.

function describe(v) {
    if (v instanceof Map) {
        return { kind: 'Map', size: v.size, entries: Array.from(v, ([k, x]) => [String(k), String(x)]) };
    }
    if (v instanceof Set) {
        return { kind: 'Set', size: v.size, values: Array.from(v, (x) => String(x)) };
    }
    if (v instanceof Date) {
        return { kind: 'Date', time: v.getTime() };
    }
    if (v instanceof RegExp) {
        return { kind: 'RegExp', source: v.source, flags: v.flags, lastIndex: v.lastIndex };
    }
    if (v instanceof Error) {
        return { kind: 'Error', name: v.name, message: v.message,
                 stack: String(v.stack || ''), isTypeError: v instanceof TypeError };
    }
    if (v instanceof DataView) {
        return { kind: 'DataView', byteOffset: v.byteOffset, byteLength: v.byteLength,
                 first: v.getInt32(0) };
    }
    return { kind: Object.prototype.toString.call(v), keys: Object.keys(v || {}) };
}

self.onmessage = (e) => {
    const seen = {};
    for (const key of Object.keys(e.data)) seen[key] = describe(e.data[key]);
    self.postMessage({ seen, echoed: e.data });
};
