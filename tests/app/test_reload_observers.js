// A reload takes the page's observers with it. observers.js (the
// Mutation/Resize/IntersectionObserver classes) is installed once and outlives
// every page, and it kept the old page's observers on its lists: their targets,
// the old document's detached nodes, read as resized each frame and the new
// page logged 'ResizeObserver loop completed with undelivered notifications'
// every frame, while an old callback could still run after the page's code was
// unloaded. After the reload only the new page's observers are heard.
//
// An inner bro-headless boots a generated app that observes one element three
// ways and logs each callback with its boot number; it resizes the viewport
// after the reload so every resize observer has something to report.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-reload-observers-' + process.pid);
const app = path.join(root, 'app');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');
fs.writeFileSync(path.join(app, 'main.js'),
    'const boot = Number(process.env.RO_BOOT || 0) + 1;\n' +
    'process.env.RO_BOOT = String(boot);\n' +
    'const el = document.createElement("div");\n' +
    'el.style.width = "50%"; el.style.height = "20px";\n' +
    'document.body.appendChild(el);\n' +
    'new ResizeObserver(() => console.log("SEEN resize boot=" + boot)).observe(el);\n' +
    'new IntersectionObserver(() => console.log("SEEN intersect boot=" + boot)).observe(el);\n' +
    'new MutationObserver(() => console.log("SEEN mutate boot=" + boot)).observe(document.body, { attributes: true, subtree: true });\n' +
    'window.poke = () => el.setAttribute("data-n", String(Math.random()));\n');

const steps = [
    'runFrames(5, { stepMs: 16 })',
    'window.poke()',
    'runFrames(2, { stepMs: 16 })',
    'console.log("RELOADING")',
    'location.reload()',
    'runFrames(5, { stepMs: 16 })',
    'for (let i = 0; i < 6; i++) { resize(700 - i * 10, 600); window.poke(); runFrames(3, { stepMs: 16 }); }',
    'console.log("END")',
].join(';\n');

const env = Object.assign({}, process.env);
delete env.RO_BOOT;
const run = cp.spawnSync(process.execPath, ['--width', '800', '--height', '600', app, '-e', steps],
                         { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-2000));
const [before, after] = out.split('RELOADING');
assert(after !== undefined && /END/.test(after), 'the run reloaded and finished:\n' + out.slice(-1500));
for (const kind of ['resize', 'intersect', 'mutate']) {
    assert(new RegExp(`SEEN ${kind} boot=1`).test(before), `the first page's ${kind} observer fired`);
    assert(new RegExp(`SEEN ${kind} boot=2`).test(after), `the reloaded page's ${kind} observer fires`);
    assert(!new RegExp(`SEEN ${kind} boot=1`).test(after), `the old page's ${kind} observer is silent after the reload`);
}
assert(!/undelivered notifications/.test(out), 'no resize loop error after the reload:\n' + after.slice(-1500));

console.log('reload observers OK');
