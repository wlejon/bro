// A reload ends the page's workers with it: a reply a worker sent just before
// the reload is never delivered to the old page's handler, whose module the
// reload unloads. It used to be: helmapps Images, zoomed in so its decoder
// worker sent the full-resolution picture, crashed reading its unloaded
// viewer when the reply landed after a reload.
//
// An inner bro-headless boots a generated app whose worker answers at once;
// the page logs each answer with its boot number. A first question is
// answered (the worker works); a second is asked, its answer left queued, and
// the page reloads before a frame delivers it.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-reload-worker-' + process.pid);
const app = path.join(root, 'app');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');
fs.writeFileSync(path.join(app, 'w.js'), 'onmessage = (e) => postMessage(e.data);\n');
fs.writeFileSync(path.join(app, 'main.js'),
    'const boot = Number(process.env.RW_BOOT || 0) + 1;\n' +
    'process.env.RW_BOOT = String(boot);\n' +
    'const state = { boot, seen: [] };\n' +
    'const w = new Worker("w.js");\n' +
    'w.onmessage = (e) => { state.seen.push(e.data); process.env.RW_SEEN = String(e.data);\n' +
    '  console.log("ANSWER boot=" + state.boot + " q=" + e.data); };\n' +
    'window.ask = (q) => w.postMessage(q);\n');

const steps = [
    'runFrames(5, { stepMs: 16 })',
    'window.ask(1)',
    // A worker takes a moment to start: until it has answered.
    'for (let i = 0; i < 1000 && process.env.RW_SEEN !== "1"; i++) { wallSleep(10); runFrames(1, { stepMs: 16 }); }',
    'window.ask(2)',
    'wallSleep(300)',              // the answer is queued for the page, not yet delivered
    'location.reload()',
    'for (let i = 0; i < 20; i++) runFrames(3, { stepMs: 16 })',
    'console.log("END boot=" + process.env.RW_BOOT)',
].join(';\n');

const env = Object.assign({}, process.env);
delete env.RW_BOOT; delete env.RW_SEEN;
const run = cp.spawnSync(process.execPath, [app, '-e', steps], { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-2000));
assert(/ANSWER boot=1 q=1/.test(out), 'the worker answered before the reload:\n' + out.slice(-1500));
assert(/END boot=2/.test(out), 'the page reloaded:\n' + out.slice(-1500));
assert(!/q=2/.test(out), 'the answer queued at the reload never reached the old page:\n' + out.slice(-1500));

console.log('reload worker reply OK');
