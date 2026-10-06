// Headless test for bro.thumb
assert(typeof bro.thumb === 'object', 'bro.thumb namespace exists');
if (!bro.thumb.available) {
    skipTest('bro.thumb is compiled out of this build');
} else {
    // 1. Base directory
    const baseDir = bro.thumb.getBaseDir();
    assert(typeof baseDir === 'string' && baseDir.length > 0, 'getBaseDir returns non-empty string');

    // 2. Capabilities
    const caps = bro.thumb.getCapabilities();
    assert(typeof caps === 'object' && caps !== null, 'getCapabilities returns object');
    assert(Array.isArray(caps.supportedExtensions), 'caps.supportedExtensions is array');
    assert(Array.isArray(caps.supportedMimeTypes), 'caps.supportedMimeTypes is array');
    assert(typeof caps.canExtractImages === 'boolean', 'caps.canExtractImages is boolean');

    // 3. Thumbnail path
    const p = bro.thumb.getThumbnailPath('/tmp/test_file.png', { size: 'normal' });
    assert(typeof p === 'string' && p.length > 0, 'getThumbnailPath returns non-empty string');
    assert(p.endsWith('.png'), 'getThumbnailPath returns png file path');

    // 4. Invalidation
    const invRes = bro.thumb.invalidate('/tmp/nonexistent_thumb.png');
    assert(typeof invRes === 'boolean', 'invalidate returns boolean');

    // 5. Synchronous non-existent file lookup returns null
    const nullRes = bro.thumb.getSync('/tmp/nonexistent_file_xyz123.png');
    assert(nullRes === null, 'getSync on nonexistent file returns null');

    // 6. Asynchronous non-existent file request returns a Promise
    const pPromise = bro.thumb.get('/tmp/nonexistent_file_xyz123.png');
    assert(typeof pPromise === 'object' && typeof pPromise.then === 'function', 'bro.thumb.get returns a Promise');
    pPromise.catch(() => {});

    console.log('test_thumb.js PASSED');
}
