// A watched source change fires `sourcechange` on the window, its files named
// from the app dir, before the reload: a `.bk` file is watched like a script,
// preventDefault() keeps the running page (the app swaps the change in
// itself), and an event let through reloads as before.
//
// An inner bro-headless boots a generated app and runs the windowed frame
// loop (runFrames, which starts the source watcher as a window does). The
// counts live in process.env, which outlives a reloaded page's realm.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-sourcechange-' + process.pid);
const app = path.join(root, 'app');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(path.join(app, 'kernels'), { recursive: true });

const bk = path.join(app, 'kernels', 'k.bk');
fs.writeFileSync(bk, '// v0\n');
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><head><title>sc</title></head><body><script src="main.js"></script></body></html>');
fs.writeFileSync(path.join(app, 'main.js'),
    'process.env.SC_BOOTS = String(Number(process.env.SC_BOOTS || 0) + 1);\n' +
    'window.addEventListener("sourcechange", (e) => {\n' +
    '  process.env.SC_EVENTS = (process.env.SC_EVENTS || "") + JSON.stringify(e.detail.files) + ";";\n' +
    '  if (process.env.SC_KEEP === "1") e.preventDefault();\n' +
    '});\n');

const steps = [
    'runFrames(10)',
    // Kept: the page takes the change, no reload.
    'process.env.SC_KEEP = "1"',
    'require("fs").writeFileSync(' + JSON.stringify(bk) + ', "// v1\\n")',
    'runFrames(60)',
    'console.log("KEPT=" + process.env.SC_BOOTS + "|" + process.env.SC_EVENTS)',
    // Let through: the page reloads.
    'process.env.SC_KEEP = "0"',
    'require("fs").writeFileSync(' + JSON.stringify(bk) + ', "// v2\\n")',
    'runFrames(90)',
    'console.log("PASSED=" + process.env.SC_BOOTS + "|" + process.env.SC_EVENTS)',
].join(';\n');

const env = Object.assign({}, process.env, { BRO_WATCH: '1' });
delete env.SC_BOOTS; delete env.SC_EVENTS; delete env.SC_KEEP;
const run = cp.spawnSync(process.execPath, [app, '-e', steps], { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-1500));
const kept = /KEPT=(\S+)/.exec(out);
const passed = /PASSED=(\S+)/.exec(out);
assert(kept && passed, 'inner run printed both results:\n' + out.slice(-1500));
assert(kept[1] === '1|["kernels/k.bk"];',
       'a .bk change fired sourcechange naming it and preventDefault kept the page, got ' + kept[1]);
assert(passed[1] === '2|["kernels/k.bk"];["kernels/k.bk"];',
       'a sourcechange let through reloaded the page, got ' + passed[1]);

console.log('sourcechange OK');
