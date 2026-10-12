// For tests/profiler/test_profiler_modules.js: a function in a module of its
// own, hot enough to own most of a profile's self time, all integer work so
// its samples are its own (no libm call under it).
export function crunchInModule(n) {
    let acc = 7;
    for (let i = 0; i < n; i++) acc = (acc * 31 + (i ^ (acc >>> 3))) | 0;
    return acc;
}
