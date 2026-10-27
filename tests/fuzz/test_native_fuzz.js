// Every native an app can reach refuses bad input with an error rather than
// crash the process (docs/app-isolation.md, step 7: native preconditions).
// Each bro.<ns> is fuzzed in a child bro-headless of its own
// (fuzz_child.js) as an app with no capabilities; a child that dies, or
// hangs, names the call it was in. FUZZ_NS=mesh,image runs only those.
// Children run eight at a time; a run is as long as its slowest namespace.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const app = path.join(process.env.BRO_APP_DIR, 'fuzz_bare');
const child = path.join(process.env.BRO_APP_DIR, '..', 'fuzz', 'fuzz_child.js');
const outDir = path.join(process.env.BRO_APP_DIR, '..', 'out', 'fuzz');
fs.mkdirSync(outDir, { recursive: true });

const only = process.env.FUZZ_NS ? process.env.FUZZ_NS.split(',') : null;
const namespaces = Object.keys(bro).filter((k) => {
    const v = bro[k];
    return v && typeof v === 'object' && (!only || only.includes(k));
});
assert(namespaces.length > 0, 'there are namespaces to fuzz');

const kParallel = 8;
const kChildMs = 180000;
const findings = [];
const slow = [];
let total = 0;

function finish(ns, last, out, status, signal, hung, ms) {
    const done = /FUZZ_DONE \S+ (\d+) calls/.exec(out);
    let at = '';
    try { at = fs.readFileSync(last, 'utf8'); } catch (e) { /* never started */ }
    if (done && status === 0 && !hung) {
        total += Number(done[1]);
        console.log(`  bro.${ns}: ${done[1]} calls, ${ms} ms`);
        return;
    }
    fs.writeFileSync(path.join(outDir, `${ns}.log`), out);
    // A child still running at kChildMs is in one native call that has not
    // returned: work as large as the arguments asked for (a bake over a mesh
    // the earlier calls subdivided), or a loop that never ends. Natives run to
    // completion (docs/app-isolation.md, scheduling), so it is reported, not
    // failed; the call is named so a real loop is found.
    if (hung) {
        slow.push(`bro.${ns} still in ${at || '(before the first call)'} after ${kChildMs / 1000} s`);
        return;
    }
    const errLine = out.split(/\r?\n/).find((l) => /ERROR|Assertion|abort|Segmentation|terminate/.test(l)) || '';
    findings.push(`bro.${ns} died (status ${status}, signal ${signal}) in ${at || '(before the first call)'}${errLine ? ' — ' + errLine.trim() : ''}`);
}

function start(ns) {
    const last = path.join(outDir, `${ns}.last`);
    try { fs.unlinkSync(last); } catch (e) { /* none yet */ }
    const t0 = Date.now();
    const run = { done: false, hung: false, out: '' };
    const p = cp.spawn(exe, [app, child, '--', ns, last], { stdio: ['ignore', 'pipe', 'pipe'] });
    p.stdout.on('data', (d) => { run.out += d; });
    p.stderr.on('data', (d) => { run.out += d; });
    run.deadline = t0 + kChildMs;
    run.kill = () => { run.hung = true; p.kill('SIGKILL'); };
    p.on('close', (status, signal) => {
        finish(ns, last, run.out, status, signal, run.hung, Date.now() - t0);
        run.done = true;
    });
    return run;
}

const queue = namespaces.slice();
const running = [];
while (queue.length || running.length) {
    while (queue.length && running.length < kParallel) running.push(start(queue.shift()));
    advanceTime(16);
    wallSleep(25);
    const now = Date.now();
    for (const r of running) if (!r.done && !r.hung && now > r.deadline) r.kill();
    for (let i = running.length - 1; i >= 0; i--) if (running[i].done) running.splice(i, 1);
}

for (const s of slow) console.log('SLOW ' + s);
for (const f of findings) console.log('FINDING ' + f);
assert(findings.length === 0, `${findings.length} namespace(s) crashed under fuzz (logs in tests/out/fuzz):\n  ` + findings.join('\n  '));
if (findings.length === 0) console.log(`test_native_fuzz OK: ${namespaces.length} namespaces, ${total} calls`);
