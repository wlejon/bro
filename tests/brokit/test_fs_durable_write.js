// A durable replace through brokit's fs, the way an app keeps a file it must
// not lose (an answers store): write a temporary file with writeSync of a
// string, fsyncSync it, close, rename over the old one. writeSync took only a
// typed array and fsyncSync did not exist; mkdtempSync was missing too.

const fs = require('fs');
const path = require('path');
const os = require('os');

assert(typeof fs.fsyncSync === 'function', 'fsyncSync exists');
assert(typeof fs.fdatasyncSync === 'function', 'fdatasyncSync exists');
assert(typeof fs.mkdtempSync === 'function', 'mkdtempSync exists');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bro-durable-'));
assert(fs.statSync(dir).isDirectory(), 'mkdtempSync makes a directory');
assert(/bro-durable-[A-Za-z0-9]{6}$/.test(dir), 'named prefix + six characters: ' + dir);
const other = fs.mkdtempSync(path.join(os.tmpdir(), 'bro-durable-'));
assert(other !== dir, 'each call a new one');
fs.rmSync(other, { recursive: true, force: true });

try {
    const file = path.join(dir, 'answers.json');
    fs.writeFileSync(file, '{"old":true}');

    const text = JSON.stringify({ name: 'Crème Brûlée', bird: 'Blue Jay' });
    const tmp = file + '.tmp';
    const fd = fs.openSync(tmp, 'w');
    const n = fs.writeSync(fd, text);
    assert(n === Buffer.byteLength(text), `writeSync(fd, string) answers the bytes: ${n}`);
    assert(fs.fsyncSync(fd) === undefined, 'fsyncSync answers undefined');
    fs.closeSync(fd);
    fs.renameSync(tmp, file);
    assert(fs.readFileSync(file, 'utf8') === text, 'the replaced file reads back, accents and all');

    // A position and an encoding.
    const fd2 = fs.openSync(file, 'r+');
    fs.writeSync(fd2, '7b', 0, 'hex');  // '{' written over '{'
    fs.writeSync(fd2, 'X', 2, 'latin1');
    fs.fdatasyncSync(fd2);
    fs.closeSync(fd2);
    assert(fs.readFileSync(file, 'utf8').slice(0, 3) === '{"X', 'positioned hex and latin1 writes');

    let code = null;
    try { fs.fsyncSync(fd2); } catch (e) { code = e.code; }
    assert(code === 'EBADF', 'fsyncSync on a closed fd: EBADF, got ' + code);
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}

console.log('OK — fs durable write: writeSync(string), fsyncSync, mkdtempSync');
