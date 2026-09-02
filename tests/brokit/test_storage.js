// Test localStorage / sessionStorage.

assert(typeof localStorage === 'object', 'localStorage exists');

// Clear first
localStorage.clear();
assert(localStorage.length === 0, 'cleared length 0: ' + localStorage.length);

localStorage.setItem('a', '1');
localStorage.setItem('b', 'two');
assert(localStorage.length === 2, 'length 2 after sets: ' + localStorage.length);
assert(localStorage.getItem('a') === '1', 'get a');
assert(localStorage.getItem('b') === 'two', 'get b');
assert(localStorage.getItem('missing') === null, 'missing is null');

// Coercion: non-strings should be stringified
localStorage.setItem('num', 42);
assert(localStorage.getItem('num') === '42', 'num coerced to string: ' + localStorage.getItem('num'));

// key(index)
const keys = new Set();
for (let i = 0; i < localStorage.length; i++) keys.add(localStorage.key(i));
assert(keys.has('a') && keys.has('b'), 'keys iterable');

// removeItem
localStorage.removeItem('a');
assert(localStorage.getItem('a') === null, 'removed a');
assert(localStorage.length === 2, 'length after remove: ' + localStorage.length); // b + num

// clear
localStorage.clear();
assert(localStorage.length === 0, 'cleared again');

// sessionStorage
assert(typeof sessionStorage === 'object', 'sessionStorage exists');
sessionStorage.clear();
sessionStorage.setItem('x', 'y');
assert(sessionStorage.getItem('x') === 'y', 'session get');
assert(sessionStorage.length === 1, 'session length');
sessionStorage.clear();

// Independence
localStorage.setItem('shared', 'L');
sessionStorage.setItem('shared', 'S');
assert(localStorage.getItem('shared') === 'L', 'local vs session L');
assert(sessionStorage.getItem('shared') === 'S', 'local vs session S');
localStorage.clear();
sessionStorage.clear();

// ── The file is real JSON ──────────────────────────────────────────────────
//
// localStorage persists to <appDir>/.storage.json. It used to be written by a
// hand escaper that knew four escapes, so a value carrying anything else came
// back changed. Every value below round-trips through the file AND the file
// itself parses with JSON.parse, which is the contract.

const fs = require('fs');
const path = require('path');
const storePath = path.join(bro.appDir, '.storage.json');

const awkward = {
  quotes: 'she said "hi" \\ back',
  lines: 'a\nb\r\nc\td',
  ctrl: 'bell form\f back\b nul-adjacent',
  unicode: 'héllo — ✓ 日本 😀',
  json: JSON.stringify({ nested: { arr: [1, 'two', null], s: 'q"uote' } }),
  slash: 'a/b\\/c',
  'key "with" quotes\n': 'k'
};
for (const [k, v] of Object.entries(awkward)) localStorage.setItem(k, v);
for (const [k, v] of Object.entries(awkward)) {
  assert(localStorage.getItem(k) === v, 'in-memory round trip: ' + k);
}

assert(fs.existsSync(storePath), '.storage.json exists at ' + storePath);
assert(!fs.existsSync(storePath + '.tmp'), 'the atomic write leaves no .tmp behind');
const onDisk = JSON.parse(fs.readFileSync(storePath, 'utf-8'));
for (const [k, v] of Object.entries(awkward)) {
  assert(onDisk[k] === v, 'the file is JSON.parse-able and holds the value for: ' + k +
         ' (got ' + JSON.stringify(onDisk[k]) + ')');
}
assert(Object.keys(onDisk).length === Object.keys(awkward).length,
       'the file holds exactly the stored keys: ' + Object.keys(onDisk).length);

localStorage.clear();
assert(JSON.stringify(JSON.parse(fs.readFileSync(storePath, 'utf-8'))) === '{}',
       'clear() writes an empty object');

