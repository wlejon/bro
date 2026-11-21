// A dedicated Worker's script goes through the same ahead-of-time cycle as
// the page's units (docs/apps.md, "Workers"): its first run compiles in
// process to be harvested on the worker's thread, the harvest is written as
// the worker ends and is the worker unit's profile, a sidecar says how to make
// its realm, the build makes the module in a worker realm on that profile,
// and the next run's worker runs the module. A classic and a module worker
// both, a pool of two workers of one script (one unit, one profile), a worker
// that closes itself before the page quits, and a worker made from a blob:
// URL, which is never a unit.

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_aot_worker_'));
const codeCache = path.join(root, 'code-cache');

function run(args) {
    const env = Object.assign({}, process.env, { BRO_APP_HOME: root, BRO_CODE_CACHE_DIR: codeCache });
    delete env.BRO_CODE_CACHE;
    const r = cp.spawnSync(headless, args, { encoding: 'utf-8', timeout: 120000, env });
    const out = (r.stdout || '') + (r.stderr || '');
    // The harvests are written from several threads at quit: none may crash.
    assert(!/bro-headless crash/.test(out), 'no crash in ' + args.join(' ') + ': ' + out.slice(-1500));
    return { status: r.status, out };
}

function write(file, text) {
    fs.writeFileSync(file, text);
    return file;
}

const isModule = (f) => /\.(so|dll|dylib)$/.test(f);
const listing = (dir) => fs.readdirSync(dir);
const workers = (dir) => fs.readFileSync(path.join(dir, 'aot.txt'), 'utf-8').split('\n')
    .filter((l) => /^worker\d+=/.test(l));

function main() {
    const app = path.join(root, 'app');
    const aot = path.join(root, 'aot');
    fs.mkdirSync(app);
    write(path.join(app, 'index.html'),
        '<!doctype html><html><body><script src="main.js"></script></body></html>\n');
    // Two workers of one script (a pool), a module worker, one that closes
    // itself mid-session (its harvest is written before the page's), and a
    // blob: worker.
    write(path.join(app, 'main.js'),
        'window.R = { classic: [] };\n' +
        'for (let i = 0; i < 2; i++) new Worker("classic.js").onmessage = (e) => { R.classic.push(e.data); };\n' +
        'new Worker("mod.js", { type: "module" }).onmessage = (e) => { R.mod = e.data; };\n' +
        'new Worker("early.js").onmessage = (e) => { R.early = e.data; };\n' +
        'const blob = new Blob(["postMessage(6 * 7)"], { type: "text/javascript" });\n' +
        'new Worker(URL.createObjectURL(blob)).onmessage = (e) => { R.blob = e.data; };\n');
    write(path.join(app, 'early.js'),
        'function tri(n) { return n * (n + 1) / 2; }\nlet e = 0;\nfor (let i = 0; i < 500; i++) e += tri(i);\n' +
        'postMessage(e);\nclose();\n');
    write(path.join(app, 'classic.js'),
        'function used(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return s; }\n' +
        'function neverRun(o) { return o.a + o.b * 2; }\n' +
        'let total = 0;\nfor (let k = 0; k < 2000; k++) total += used(10);\n' +
        'postMessage(total);\n');
    write(path.join(app, 'mod.js'),
        'const sq = (x) => x * x;\nlet t = 0;\nfor (let i = 0; i < 100; i++) t += sq(i);\npostMessage(t);\n');
    const probe = write(path.join(root, 'probe.js'),
        'const deadline = Date.now() + 15000;\n' +
        'while ((R.classic.length < 2 || R.mod === undefined || R.early === undefined || R.blob === undefined) &&\n' +
        '       Date.now() < deadline) {\n' +
        '    advanceTime(16); wallSleep(2);\n}\n' +
        // The page outlives the closed worker by a few frames.
        'for (let i = 0; i < 10; i++) { advanceTime(16); wallSleep(2); }\n' +
        'assert(R.classic.join() === "90000,90000", "both classic workers ran: " + R.classic);\n' +
        'assert(R.early === 20833250, "the early worker ran: " + R.early);\n' +
        'assert(R.mod === 328350, "the module worker ran: " + R.mod);\n' +
        'assert(R.blob === 42, "the blob worker ran: " + R.blob);\n');

    const installed = run([app, '--aot-build', aot]);
    if (installed.status !== 0 && process.platform === 'darwin') {
        skipTest('--aot-build failed on macOS: ' + installed.out.slice(-400));
        return;
    }
    assert(installed.status === 0, 'the AOT build succeeds: ' + installed.out.slice(-400));
    assert(workers(aot).length === 0, 'no worker unit before any run: ' + workers(aot));

    // The first run: each file worker compiles to be harvested; the blob
    // worker is no unit.
    const first = run(['--aot', aot, app, probe]);
    assert(first.status === 0, 'the first run passes: ' + first.out.slice(-800));
    const harvested = first.out.split('\n').filter((l) => /no ahead-of-time module for worker .* to be harvested/.test(l));
    assert(harvested.length === 4, 'every file worker instance is harvested: ' + harvested.join('\n'));
    const written = first.out.split('\n').filter((l) => /aot: worker .*: its run is harvested/.test(l));
    for (const name of ['classic.js', 'mod.js', 'early.js']) {
        assert(written.some((l) => l.includes(name)), name + ' wrote its harvest: ' + first.out.slice(-1200));
    }
    assert(listing(aot).filter((f) => f.endsWith('.worker')).length === 3, 'three sidecars: ' + listing(aot));
    assert(listing(aot).filter((f) => f.endsWith('.tfb')).length === 4, 'a profile per unit, the page\'s too: ' +
        listing(aot));
    assert(listing(aot).filter((f) => f.endsWith('.tmp')).length === 0, 'no harvest left in its temp file: ' +
        listing(aot));

    // The build: each worker unit built on its profile, in a worker realm.
    const built = run([app, '--aot-build', aot]);
    assert(built.status === 0, 'the build succeeds: ' + built.out.slice(-600));
    const lines = workers(aot);
    assert(lines.length === 3, 'three worker units: ' + lines);
    for (const l of lines) assert(/built \d+ ms on its profile/.test(l), 'built on its profile: ' + l);
    const classicLine = lines.find((l) => l.includes('classic.js'));
    const cold = /(\d+) of (\d+) functions cold/.exec(classicLine);
    assert(cold && Number(cold[1]) >= 1, 'the function the run never entered is cold: ' + classicLine);

    // The next run's workers run their modules: the record built in the
    // worker realm matches the worker's run.
    const second = run(['--aot', aot, app, probe]);
    assert(second.status === 0, 'the second run passes: ' + second.out.slice(-800));
    const ran = second.out.split('\n').filter((l) => /aot: worker .* runs its ahead-of-time module/.test(l));
    assert(ran.length === 4, 'every file worker instance ran its module: ' + second.out.slice(-800));
    assert(!/out of date/.test(second.out), 'no worker module is refused: ' + second.out.slice(-800));
    assert(!second.out.includes('to be harvested'), 'nothing is harvested again');

    // A build with nothing new keeps both modules.
    const kept = run([app, '--aot-build', aot]);
    assert(kept.status === 0 && workers(aot).every((l) => / current /.test(l)),
        'a second build keeps the worker modules: ' + workers(aot));
    assert(listing(aot).filter(isModule).length === 4, 'the modules stay: ' + listing(aot));

    // The worker script is gone: its unit goes at the next build.
    fs.unlinkSync(path.join(app, 'mod.js'));
    assert(run([app, '--aot-build', aot]).status === 0, 'the build after a script is removed succeeds');
    assert(workers(aot).length === 2, 'the removed worker\'s unit is gone: ' + workers(aot));
    console.log('test_aot_worker OK');
}

main();
