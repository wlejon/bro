// The page hears pagehide when the app quits, as a closed tab's does, while
// what its handler reaches still runs: an app saves what it is in the middle
// of (helmapps Recorder, a recording not yet stopped) synchronously there.
// It fires once a page: after a reload the new page gets its own at the quit.
//
// An inner bro-headless boots a generated app whose pagehide handler appends
// its boot number to a file; the script reloads once and ends.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-pagehide-quit-' + process.pid);
const app = path.join(root, 'app');
const out = path.join(root, 'hidden.txt');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');
fs.writeFileSync(path.join(app, 'main.js'),
    'const fs = require("fs");\n' +
    'const boot = Number(process.env.PQ_BOOT || 0) + 1;\n' +
    'process.env.PQ_BOOT = String(boot);\n' +
    'addEventListener("pagehide", () => fs.appendFileSync(' + JSON.stringify(out) + ', "hide boot=" + boot + "\\n"));\n');

const steps = [
    'runFrames(3, { stepMs: 16 })',
    'location.reload()',
    'for (let i = 0; i < 40 && process.env.PQ_BOOT !== "2"; i++) runFrames(5, { stepMs: 16 })',
    'console.log("END boot=" + process.env.PQ_BOOT)',
].join(';\n');

const env = Object.assign({}, process.env);
delete env.PQ_BOOT;
const run = cp.spawnSync(process.execPath, [app, '-e', steps], { encoding: 'utf8', env });
const log = String(run.stdout) + String(run.stderr);
let hidden = '';
try { hidden = fs.readFileSync(out, 'utf8'); } catch (_) { /* never written */ }
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + log.slice(-2000));
assert(/END boot=2/.test(log), 'the page reloaded:\n' + log.slice(-1500));
assert(hidden === 'hide boot=1\nhide boot=2\n',
       'pagehide once before the reload and once at the quit, each page its own: ' + JSON.stringify(hidden));

console.log('pagehide at quit OK');
