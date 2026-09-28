// The on-disk code cache (docs/code-cache.md), end to end: an inner
// bro-headless boots a small app twice against a private cache directory. The
// first boot compiles and stores, the second is served from the cache and runs
// the same; an edited module, a damaged entry and a new file under an import()
// glob each miss and still run correctly; BRO_CODE_CACHE=0 turns it off.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-code-cache-test-' + process.pid);
const app = path.join(root, 'app');
const cacheDir = path.join(root, 'cache');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(path.join(app, 'js', 'parts'), { recursive: true });

fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><div id="out"></div>' +
    '<script type="module">import "./js/main.js";</script></body></html>');
fs.writeFileSync(path.join(app, 'js', 'dep.js'),
    'export class Acc { constructor(n) { this.n = n; } add(k) { this.n += k; return this; } }\n' +
    'export function value() { return 10; }\n');
fs.writeFileSync(path.join(app, 'js', 'parts', 'a.js'), 'export const part = 1;\n');
fs.writeFileSync(path.join(app, 'js', 'main.js'),
    'import { Acc, value } from "./dep.js";\n' +
    'const name = globalThis.__ccPart || "a";\n' +
    'globalThis.__ccValue = new Acc(value()).add(5).n;\n' +
    'import(`./parts/${name}.js`).then((m) => { globalThis.__ccPartValue = m.part; });\n');

function boot(extraEnv) {
    const env = Object.assign({}, process.env, { BRO_CODE_CACHE_DIR: cacheDir }, extraEnv || {});
    const inner = "console.log('CCVALUE=' + globalThis.__ccValue);";
    // The log (and the compile lines in it) is on stderr.
    const run = cp.spawnSync(process.execPath, [app, '-e', inner], { encoding: 'utf8', env });
    const out = String(run.stdout) + String(run.stderr);
    assert(run.status === 0, 'inner run exited cleanly: ' + run.status + ' ' + out.slice(-400));
    const value = /CCVALUE=(\S+)/.exec(out);
    assert(value, 'inner run printed its value: ' + out.slice(-400));
    const page = /compiled \S*index\.html in \d+ ms \(code cache ([a-z]+)(?:: ([^)]*))?\)/.exec(out);
    return { value: Number(value[1]), status: page ? page[1] : 'none', note: page ? (page[2] || '') : '', out };
}

function entries() {
    return fs.existsSync(cacheDir) ? fs.readdirSync(cacheDir).filter((f) => f.endsWith('.bzc')) : [];
}

let r = boot();
assert(r.status === 'miss' && r.note === 'no entry', 'first boot misses: ' + r.status + ' ' + r.note);
assert(r.value === 15, 'first boot runs: ' + r.value);
assert(entries().length >= 1, 'the first boot stored an entry');

r = boot();
assert(r.status === 'hit', 'second boot hits: ' + r.status + ' ' + r.note);
assert(r.value === 15, 'the cached program runs the same: ' + r.value);

fs.writeFileSync(path.join(app, 'js', 'dep.js'),
    'export class Acc { constructor(n) { this.n = n; } add(k) { this.n += k; return this; } }\n' +
    'export function value() { return 20; }\n');
r = boot();
assert(r.status === 'miss' && r.note.startsWith('source changed'), 'an edited module misses: ' + r.note);
assert(r.value === 25, 'the edit is what runs: ' + r.value);
r = boot();
assert(r.status === 'hit' && r.value === 25, 'the miss re-stored the entry: ' + r.status);

fs.writeFileSync(path.join(app, 'js', 'parts', 'b.js'), 'export const part = 2;\n');
r = boot();
assert(r.status === 'miss' && r.note === 'module resolution changed', 'a new glob file misses: ' + r.note);

// Every entry damaged: truncated to half. Each is a miss, never a crash.
for (const f of entries()) {
    const p = path.join(cacheDir, f);
    const bytes = fs.readFileSync(p);
    fs.writeFileSync(p, bytes.subarray(0, bytes.length >> 1));
}
r = boot();
assert(r.status === 'miss' && r.note === 'truncated', 'a truncated entry misses: ' + r.note);
assert(r.value === 25, 'and the program still runs: ' + r.value);
r = boot();
assert(r.status === 'hit', 'the damaged entry was replaced: ' + r.status);

r = boot({ BRO_CODE_CACHE: '0' });
assert(r.status === 'none' && r.value === 25, 'BRO_CODE_CACHE=0 compiles without the cache: ' + r.status);

fs.rmSync(root, { recursive: true, force: true });
console.log('code cache OK');
