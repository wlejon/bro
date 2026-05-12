// Test fs sync APIs.

const fs = require('fs');
const path = require('path');

assert(typeof fs === 'object', 'fs require works');
assert(typeof fs.writeFileSync === 'function', 'writeFileSync');
assert(typeof fs.readFileSync === 'function', 'readFileSync');
assert(typeof fs.existsSync === 'function', 'existsSync');

const tmpDir = 'D:/projects/bro/tests/brokit/tmp';
try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
fs.mkdirSync(tmpDir, { recursive: true });
assert(fs.existsSync(tmpDir), 'tmpdir exists after mkdir');

try {
    // String write/read
    const f1 = tmpDir + '/a.txt';
    fs.writeFileSync(f1, 'hello world', 'utf-8');
    assert(fs.existsSync(f1), 'a.txt exists');
    const back = fs.readFileSync(f1, 'utf-8');
    assert(back === 'hello world', 'read string back: ' + back);

    // Read without encoding → ArrayBuffer (per docs)
    const buf = fs.readFileSync(f1);
    // brokit may return ArrayBuffer or Uint8Array; both are acceptable
    let len;
    if (buf instanceof ArrayBuffer) len = buf.byteLength;
    else if (buf && typeof buf.length === 'number') len = buf.length;
    else len = -1;
    assert(len === 11, 'binary read length: ' + len + ' typeof ' + (buf && buf.constructor && buf.constructor.name));

    // Write Uint8Array
    const f2 = tmpDir + '/b.bin';
    const u = new Uint8Array([1, 2, 3, 4, 5]);
    fs.writeFileSync(f2, u);
    const back2 = fs.readFileSync(f2);
    let bv;
    if (back2 instanceof ArrayBuffer) bv = new Uint8Array(back2);
    else bv = back2;
    assert(bv.length === 5, 'bin length: ' + bv.length);
    assert(bv[0] === 1 && bv[4] === 5, 'bin contents');

    // mkdir recursive
    const deep = tmpDir + '/x/y/z';
    fs.mkdirSync(deep, { recursive: true });
    assert(fs.existsSync(deep), 'deep mkdir');

    // readdirSync
    fs.writeFileSync(deep + '/leaf.txt', 'leaf');
    const entries = fs.readdirSync(deep);
    assert(Array.isArray(entries), 'readdir is array');
    assert(entries.indexOf('leaf.txt') >= 0, 'leaf.txt in readdir: ' + entries.join(','));

    // statSync
    if (typeof fs.statSync === 'function') {
        const st = fs.statSync(f1);
        assert(typeof st === 'object', 'statSync returns obj');
        assert(typeof st.size === 'number', 'stat size: ' + st.size);
        assert(st.size === 11, 'stat size correct: ' + st.size);
        assert(typeof st.isFile === 'function', 'isFile is fn');
        assert(st.isFile() === true, 'isFile true');
        const stD = fs.statSync(tmpDir);
        assert(stD.isDirectory() === true, 'isDirectory true');
    }

    // existsSync of missing
    assert(fs.existsSync(tmpDir + '/nope') === false, 'missing existsSync false');

    // readFileSync of missing — should throw
    let threw = false;
    try { fs.readFileSync(tmpDir + '/nope'); } catch (e) { threw = true; }
    assert(threw, 'readFileSync missing throws');

    // appendFileSync
    if (typeof fs.appendFileSync === 'function') {
        fs.appendFileSync(f1, '!', 'utf-8');
        const v = fs.readFileSync(f1, 'utf-8');
        assert(v === 'hello world!', 'append: ' + v);
    }

    // unlinkSync
    fs.unlinkSync(f1);
    assert(!fs.existsSync(f1), 'unlinked');
} finally {
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
}
assert(!fs.existsSync(tmpDir), 'tmp cleaned up');
