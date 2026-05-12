// Test console methods are callable and don't throw.

assert(typeof console === 'object', 'console exists');
assert(typeof console.log === 'function', 'console.log is fn');
assert(typeof console.warn === 'function', 'console.warn is fn');
assert(typeof console.error === 'function', 'console.error is fn');
assert(typeof console.info === 'function', 'console.info is fn');
assert(typeof console.debug === 'function', 'console.debug is fn');
assert(typeof console.assert === 'function', 'console.assert is fn');
assert(typeof console.time === 'function', 'console.time is fn');
assert(typeof console.timeEnd === 'function', 'console.timeEnd is fn');
assert(typeof console.timeLog === 'function', 'console.timeLog is fn');

// Smoke calls — none should throw.
try {
    console.log('log', 1, { a: 2 }, [3, 4]);
    console.warn('warn');
    console.error('error');
    console.info('info');
    console.debug('debug');
    console.assert(true, 'true should not log');
    console.assert(false, 'false should log but not throw');
    console.time('lbl');
    console.timeLog('lbl', 'mid');
    console.timeEnd('lbl');
} catch (e) {
    assert(false, 'console call threw: ' + e.message);
}
