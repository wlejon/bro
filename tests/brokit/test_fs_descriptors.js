// fs file descriptors: openSync / readSync / writeSync / fstatSync / closeSync.
//
// Before these existed, readFileSync was the only way to get bytes out of a
// file, and it returns all of them. That is a ceiling rather than an
// inefficiency: a script that wants one dataset out of a multi-gigabyte HDF5
// container, or one frame out of a video, has no way to ask for a range, and no
// amount of JS works around a missing syscall.
//
// The cases below are the ones that bite. `position: null` means "continue from
// where the last read stopped", which is what makes sequential reading work.
// Seeking backwards after a short read at EOF has to work too — a stream that
// hit EOF refuses to seek until its error flags are cleared, so without that
// one line the first partial read turns a descriptor permanently dead.

const fs = require('fs');
const path = require('path');
const os = require('os');

assert(typeof fs.openSync === 'function', 'openSync exists');
assert(typeof fs.readSync === 'function', 'readSync exists');
assert(typeof fs.writeSync === 'function', 'writeSync exists');
assert(typeof fs.fstatSync === 'function', 'fstatSync exists');
assert(typeof fs.closeSync === 'function', 'closeSync exists');

const tmpDir = path.join(os.tmpdir(), 'bro-fs-descriptors-test');
try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
fs.mkdirSync(tmpDir, { recursive: true });

const file = path.join(tmpDir, 'bytes.bin');

try {
    // 256 distinct bytes, so any offset error shows up as a wrong value rather
    // than as a plausible one.
    const src = new Uint8Array(256);
    for (let i = 0; i < 256; i++) src[i] = i;
    fs.writeFileSync(file, src);

    // ---- size ------------------------------------------------------------
    let fd = fs.openSync(file, 'r');
    assert(typeof fd === 'number' && fd >= 0, 'openSync returns a descriptor');
    assert(fs.fstatSync(fd).size === 256, 'fstatSync reports the size');

    // ---- positional reads ------------------------------------------------
    const buf = new Uint8Array(16);
    let n = fs.readSync(fd, buf, 0, 16, 100);
    assert(n === 16, `read 16 bytes at 100, got ${n}`);
    for (let i = 0; i < 16; i++) {
        assert(buf[i] === 100 + i, `byte at 100+${i} is ${100 + i}, got ${buf[i]}`);
    }

    // Seeking backwards must work.
    n = fs.readSync(fd, buf, 0, 8, 0);
    assert(n === 8, 'read 8 bytes at 0');
    for (let i = 0; i < 8; i++) assert(buf[i] === i, `byte ${i} after seeking back`);

    // ---- position null continues from the last read -----------------------
    n = fs.readSync(fd, buf, 0, 8, null);
    assert(n === 8, 'read 8 more with position null');
    for (let i = 0; i < 8; i++) {
        assert(buf[i] === 8 + i, `continued read byte ${8 + i}, got ${buf[i]}`);
    }

    // ---- offset into the destination buffer --------------------------------
    const dest = new Uint8Array(32).fill(0xEE);
    n = fs.readSync(fd, dest, 8, 4, 200);
    assert(n === 4, 'read 4 bytes into offset 8');
    assert(dest[7] === 0xEE, 'byte before the destination offset untouched');
    assert(dest[8] === 200 && dest[11] === 203, 'bytes landed at the right offset');
    assert(dest[12] === 0xEE, 'byte after the read untouched');

    // ---- a short read at EOF must not kill the descriptor ------------------
    const tail = new Uint8Array(32);
    n = fs.readSync(fd, tail, 0, 32, 240);
    assert(n === 16, `only 16 bytes left from 240, got ${n}`);
    assert(tail[0] === 240 && tail[15] === 255, 'the tail bytes are right');

    n = fs.readSync(fd, buf, 0, 4, 0);
    assert(n === 4, 'the descriptor still works after a short read at EOF');
    assert(buf[0] === 0 && buf[3] === 3, 'and reads the right bytes');

    // Reading entirely past the end returns zero rather than throwing.
    n = fs.readSync(fd, buf, 0, 16, 1000);
    assert(n === 0, `reading past the end returns 0, got ${n}`);

    fs.closeSync(fd);

    // ---- bounds are checked ------------------------------------------------
    fd = fs.openSync(file, 'r');
    let threw = false;
    try { fs.readSync(fd, buf, 8, 16, 0); } catch (e) { threw = true; }
    assert(threw, 'offset + length beyond the buffer throws');
    fs.closeSync(fd);

    // ---- a closed descriptor is bad ---------------------------------------
    threw = false;
    try { fs.readSync(fd, buf, 0, 4, 0); } catch (e) { threw = e.code === 'EBADF'; }
    assert(threw, 'reading a closed descriptor throws EBADF');

    threw = false;
    try { fs.openSync(path.join(tmpDir, 'nope.bin'), 'r'); }
    catch (e) { threw = e.code === 'ENOENT'; }
    assert(threw, 'opening a missing file throws ENOENT');

    // ---- writing -----------------------------------------------------------
    const wfile = path.join(tmpDir, 'written.bin');
    let wfd = fs.openSync(wfile, 'w');
    const payload = new Uint8Array([10, 20, 30, 40, 50, 60, 70, 80]);
    assert(fs.writeSync(wfd, payload, 0, 8, 0) === 8, 'writeSync wrote 8 bytes');
    // Overwrite in the middle, positionally.
    assert(fs.writeSync(wfd, new Uint8Array([99, 98]), 0, 2, 3) === 2, 'positional overwrite');
    fs.closeSync(wfd);

    const readBack = fs.readFileSync(wfile);
    assert(readBack.length === 8, `written file is 8 bytes, got ${readBack.length}`);
    assert(readBack[0] === 10 && readBack[2] === 30, 'untouched bytes survive');
    assert(readBack[3] === 99 && readBack[4] === 98, 'positional overwrite landed');
    assert(readBack[5] === 60 && readBack[7] === 80, 'bytes after the overwrite survive');

    // A read-only descriptor must refuse to write rather than silently do
    // nothing, which is the failure mode that costs an afternoon.
    const rfd = fs.openSync(wfile, 'r');
    threw = false;
    try { fs.writeSync(rfd, payload, 0, 4, 0); } catch (e) { threw = e.code === 'EBADF'; }
    assert(threw, 'writing to a read-only descriptor throws EBADF');
    fs.closeSync(rfd);

    // ---- several descriptors on one file are independent -------------------
    const a = fs.openSync(file, 'r');
    const b = fs.openSync(file, 'r');
    assert(a !== b, 'two opens give two descriptors');
    fs.readSync(a, buf, 0, 4, 0);
    fs.readSync(b, buf, 0, 4, 128);
    assert(buf[0] === 128, 'the second descriptor has its own position');
    const contA = new Uint8Array(4);
    fs.readSync(a, contA, 0, 4, null);
    assert(contA[0] === 4, "the first descriptor's position was not disturbed");
    fs.closeSync(a);
    fs.closeSync(b);

} finally {
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
}

console.log('OK — fs descriptors: positional read/write, EOF, bounds, independence');
