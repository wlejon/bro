// A watched change of stylesheets alone restyles the running page instead of
// reloading it: the DOM and the page's script state stay, the linked sheet's
// new rules apply, and so do those of a sheet it @imports. A change that
// names a script too reloads as before, and the old page's `pagehide` puts
// what it was in the middle of into sessionStorage, which the new page reads.
//
// An inner bro-headless boots a generated app and runs the windowed frame
// loop (runFrames, which starts the source watcher as a window does). The
// boot count lives in process.env, which outlives a reloaded page's realm.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-css-in-place-' + process.pid);
const app = path.join(root, 'app');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(path.join(app, 'css'), { recursive: true });

const css = path.join(app, 'css', 'style.css');
const imported = path.join(app, 'css', 'more.css');
const js = path.join(app, 'main.js');
fs.writeFileSync(css, '@import "more.css";\n#box { width: 100px; height: 10px; }\n');
fs.writeFileSync(imported, '#box { margin-left: 5px; }\n');
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><head><title>css</title><link rel="stylesheet" href="css/style.css"></head>' +
    '<body><div id="box"></div><script src="main.js"></script></body></html>');
fs.writeFileSync(js,
    'process.env.CSS_BOOTS = String(Number(process.env.CSS_BOOTS || 0) + 1);\n' +
    'window.kept = (window.kept || 0) + 1;\n' +
    'document.getElementById("box").dataset.made = "by-script";\n' +
    'window.restored = sessionStorage.getItem("draft") || "none";\n' +
    'addEventListener("pagehide", () => sessionStorage.setItem("draft", "kept-" + window.kept));\n');

const probe = (tag) =>
    'console.log("' + tag + '=" + [process.env.CSS_BOOTS, window.kept, ' +
    'document.getElementById("box").dataset.made, ' +
    'document.getElementById("box").getBoundingClientRect().width, ' +
    'document.getElementById("box").getBoundingClientRect().left, window.restored].join("|"))';
const write = (file, text) => 'require("fs").writeFileSync(' + JSON.stringify(file) + ', ' + JSON.stringify(text) + ')';

const steps = [
    'runFrames(10)',
    probe('BOOT'),
    'window.kept = 41',
    write(css, '@import "more.css";\n#box { width: 200px; height: 10px; }\n'),
    'runFrames(60)',
    probe('LINKED'),
    write(imported, '#box { margin-left: 30px; }\n'),
    'runFrames(60)',
    probe('IMPORTED'),
    write(css, '@import "more.css";\n#box { width: 300px; height: 10px; }\n'),
    write(js, require('fs').readFileSync(js, 'utf8') + '// touched\n'),
    'runFrames(90)',
    probe('SCRIPT'),
].join(';\n');

const env = Object.assign({}, process.env, { BRO_WATCH: '1' });
delete env.CSS_BOOTS;
const run = cp.spawnSync(process.execPath, [app, '-e', steps], { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-1500));
const got = (tag) => { const m = new RegExp(tag + '=(\\S+)').exec(out); return m ? m[1] : null; };
// Each field: boots | window.kept | the script's data attribute | width |
// left | what the page restored from sessionStorage.
const left = (px) => String(8 + px);   // the body's UA margin, then the box's own
assert(got('BOOT') === '1|1|by-script|100|' + left(5) + '|none', 'the page booted with both sheets: ' + got('BOOT'));
assert(got('LINKED') === '1|41|by-script|200|' + left(5) + '|none',
       'a linked sheet edited: restyled, the page and its state kept: ' + got('LINKED') + '\n' + out.slice(-1500));
assert(got('IMPORTED') === '1|41|by-script|200|' + left(30) + '|none',
       'an @imported sheet edited: read again, the page kept: ' + got('IMPORTED'));
assert(got('SCRIPT') === '2|1|by-script|300|' + left(30) + '|kept-41',
       'a change naming a script reloads the page, which reads what pagehide saved: ' + got('SCRIPT'));
assert(/restyled the running page/.test(out), 'the log says the page was restyled in place');

console.log('css in place OK');
