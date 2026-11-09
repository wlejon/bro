// A reload drops the page's pending fs.promises operations: a read the old page
// started just before the reload never settles into its code, whose module the
// reload unloads. brokit's async fs settles on the JS thread at its pump, and
// that pump outlives the page, so the old page's `.then` used to run after the
// reload (reading the unloaded module's state).
//
// An inner bro-headless boots a generated app that reads a file through
// fs.promises when asked and logs the result with its boot number. A first
// read settles on the first page (reads work); a second is started, its result
// left waiting for the pump, and the page reloads before a frame settles it.
// The new page reads at boot, which shows fs.promises still works after.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-reload-fspromise-' + process.pid);
const app = path.join(root, 'app');
const file = path.join(root, 'data.txt');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(file, 'hello');
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');
fs.writeFileSync(path.join(app, 'main.js'),
    'const fs = require("fs");\n' +
    'const boot = Number(process.env.FP_BOOT || 0) + 1;\n' +
    'process.env.FP_BOOT = String(boot);\n' +
    'const state = { boot, reads: 0 };\n' +
    'window.read = (q) => fs.promises.readFile(' + JSON.stringify(file) + ', "utf8").then((t) => {\n' +
    '  state.reads++; process.env.FP_DONE = String(q);\n' +
    '  console.log("READ boot=" + state.boot + " q=" + q + " text=" + t);\n' +
    '});\n' +
    'if (boot === 2) window.read(3);\n');

const steps = [
    'runFrames(5, { stepMs: 16 })',
    'window.read(1)',
    'for (let i = 0; i < 100 && process.env.FP_DONE !== "1"; i++) { wallSleep(10); runFrames(1, { stepMs: 16 }); }',
    'window.read(2)',
    'wallSleep(200)',              // the read is done on the pool, its promise not yet settled
    'location.reload()',
    'for (let i = 0; i < 20; i++) runFrames(3, { stepMs: 16 })',
    'console.log("END boot=" + process.env.FP_BOOT)',
].join(';\n');

const env = Object.assign({}, process.env);
delete env.FP_BOOT; delete env.FP_DONE;
const run = cp.spawnSync(process.execPath, [app, '-e', steps], { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-2000));
assert(/READ boot=1 q=1 text=hello/.test(out), 'the first page read the file:\n' + out.slice(-1500));
assert(/END boot=2/.test(out), 'the page reloaded:\n' + out.slice(-1500));
assert(/READ boot=2 q=3 text=hello/.test(out), 'the new page reads after the reload:\n' + out.slice(-1500));
assert(!/q=2/.test(out), 'the read pending at the reload never settled into the old page:\n' + out.slice(-1500));
assert(!/Hard runtime error/.test(out), 'no runtime error:\n' + out.slice(-1500));

console.log('reload fs.promises OK');
