// Test navigator global.

assert(typeof navigator === 'object', 'navigator exists');

assert(typeof navigator.userAgent === 'string', 'userAgent string: ' + navigator.userAgent);
assert(navigator.userAgent.length > 0, 'userAgent nonempty');

assert(typeof navigator.platform === 'string', 'platform string: ' + navigator.platform);

assert(typeof navigator.language === 'string', 'language string: ' + navigator.language);

// hardwareConcurrency (user spec mentions this)
if ('hardwareConcurrency' in navigator) {
    assert(typeof navigator.hardwareConcurrency === 'number', 'hardwareConcurrency number');
    assert(navigator.hardwareConcurrency > 0, 'hwc > 0: ' + navigator.hardwareConcurrency);
} else {
    console.warn('navigator.hardwareConcurrency missing'); // BUG: navigator.hardwareConcurrency-missing
}

// languages
if ('languages' in navigator) {
    assert(Array.isArray(navigator.languages), 'languages is array');
}

// onLine
if ('onLine' in navigator) {
    assert(typeof navigator.onLine === 'boolean', 'onLine bool: ' + navigator.onLine);
}
