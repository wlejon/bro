// A reload ends the page's fs.watch watchers with it: after the reload only the
// new page's listener hears a change. brokit's watchers (native and the JS
// facade's registry) are installed once and outlive the page, and the old
// page's listener used to run after its module was unloaded and abort the
// process ('a property read on an unknown object kind'); helmapps Term, which
// watches its settings dir, logged 'term: watch: an object is not a function'
// after a reload and crashed at teardown.
//
// An inner bro-headless boots a generated app that watches a directory and
// logs each change with its boot number and the file's name. A change before
// the reload reaches the first page (the watcher works); the files written
// after it reach the second page only.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-reload-fswatch-' + process.pid);
const app = path.join(root, 'app');
const dir = path.join(root, 'watched');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(app, { recursive: true });
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');
fs.writeFileSync(path.join(app, 'main.js'),
    'const fs = require("fs");\n' +
    'const boot = Number(process.env.FW_BOOT || 0) + 1;\n' +
    'process.env.FW_BOOT = String(boot);\n' +
    'const state = { boot, n: 0 };\n' +
    'fs.watch(' + JSON.stringify(dir) + ', {}, (type, file) => {\n' +
    '  state.n++; process.env.FW_SEEN = String(file);\n' +
    '  console.log("WATCH boot=" + state.boot + " file=" + file);\n' +
    '});\n');

const d = JSON.stringify(dir);
const steps = [
    'const fs = require("fs")',
    'runFrames(5, { stepMs: 16 })',
    'fs.writeFileSync(' + d + ' + "/pre.txt", "x")',
    'for (let i = 0; i < 100 && process.env.FW_SEEN !== "pre.txt"; i++) { wallSleep(10); runFrames(1, { stepMs: 16 }); }',
    'location.reload()',
    'for (let i = 0; i < 50 && process.env.FW_BOOT !== "2"; i++) runFrames(5, { stepMs: 16 })',
    'for (let i = 0; i < 5; i++) { fs.writeFileSync(' + d + ' + "/f" + i + ".txt", "x" + i); wallSleep(50); runFrames(3, { stepMs: 16 }); }',
    'console.log("END boot=" + process.env.FW_BOOT)',
].join(';\n');

const env = Object.assign({}, process.env);
delete env.FW_BOOT; delete env.FW_SEEN;
const run = cp.spawnSync(process.execPath, [app, '-e', steps], { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-2000));
assert(/WATCH boot=1 file=pre\.txt/.test(out), 'the first page heard a change:\n' + out.slice(-1500));
assert(/END boot=2/.test(out), 'the page reloaded:\n' + out.slice(-1500));
assert(/WATCH boot=2 file=f\d\.txt/.test(out), 'the new page heard the changes:\n' + out.slice(-1500));
assert(!/WATCH boot=1 file=f/.test(out), 'the old page\'s listener never ran after the reload:\n' + out.slice(-1500));
assert(!/Hard runtime error/.test(out), 'no runtime error:\n' + out.slice(-1500));

console.log('reload fs.watch OK');
