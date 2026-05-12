// Test path module.

const path = require('path');
assert(typeof path === 'object', 'path require');
assert(typeof path.join === 'function', 'join');
assert(typeof path.resolve === 'function', 'resolve');

// join
assert(path.join('a', 'b', 'c').replace(/\\/g, '/') === 'a/b/c', 'join: ' + path.join('a', 'b', 'c'));
assert(path.join('a', '..', 'b').replace(/\\/g, '/') === 'b', 'join with ..: ' + path.join('a', '..', 'b'));

// resolve returns absolute
const r = path.resolve('a');
assert(r.length > 1, 'resolve absolute: ' + r);
// On Windows it should start with drive letter
if (process.platform === 'win32') {
    assert(/^[A-Za-z]:/.test(r), 'win resolve has drive: ' + r);
}

// basename
assert(path.basename('/foo/bar.txt') === 'bar.txt', 'basename: ' + path.basename('/foo/bar.txt'));
assert(path.basename('/foo/bar.txt', '.txt') === 'bar', 'basename with ext: ' + path.basename('/foo/bar.txt', '.txt'));

// dirname
const d = path.dirname('/foo/bar/baz.txt').replace(/\\/g, '/');
assert(d === '/foo/bar', 'dirname: ' + d);

// extname
assert(path.extname('foo.txt') === '.txt', 'extname: ' + path.extname('foo.txt'));
assert(path.extname('foo') === '', 'extname empty: "' + path.extname('foo') + '"');
assert(path.extname('foo.tar.gz') === '.gz', 'extname multi: ' + path.extname('foo.tar.gz'));

// normalize
const n = path.normalize('/foo/./bar/../baz').replace(/\\/g, '/');
assert(n === '/foo/baz', 'normalize: ' + n);

// sep
assert(path.sep === '\\' || path.sep === '/', 'sep: ' + path.sep);
if (process.platform === 'win32') {
    assert(path.sep === '\\', 'win sep: ' + path.sep);
}

// posix / win32 namespaces
if (path.posix) {
    assert(path.posix.sep === '/', 'posix.sep: ' + path.posix.sep);
    assert(path.posix.join('a', 'b') === 'a/b', 'posix join: ' + path.posix.join('a', 'b'));
} else {
    console.warn('path.posix missing'); // BUG: path.posix-missing
}
if (path.win32) {
    assert(path.win32.sep === '\\', 'win32.sep: ' + path.win32.sep);
} else {
    console.warn('path.win32 missing'); // BUG: path.win32-missing
}

// isAbsolute
if (typeof path.isAbsolute === 'function') {
    if (process.platform === 'win32') {
        assert(path.isAbsolute('C:\\foo') === true, 'isAbsolute win drive');
        assert(path.isAbsolute('foo') === false, 'isAbsolute relative');
    }
}

// parse / format
if (typeof path.parse === 'function') {
    const p = path.parse('/a/b/c.txt');
    assert(p.base === 'c.txt', 'parse.base: ' + p.base);
    assert(p.ext === '.txt', 'parse.ext: ' + p.ext);
    assert(p.name === 'c', 'parse.name: ' + p.name);
}
