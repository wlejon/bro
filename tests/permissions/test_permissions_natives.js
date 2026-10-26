// The natives that take a path of their own (docs/apps.md, Permissions): every
// library's natives ask the one access gate (bronze's embed/embed_access.h),
// so a path in the user's home outside the app's own folders is refused
// without `files` and reached with it, whichever library the native is in:
// bro's own (headless writeFile/screenshot, VideoEncoder, GifEncoder,
// bro.media, bro.window.captureScreen, <img>), the desktop siblings (bro.vfs,
// bro.search, bro.thumb, bro.apps' recent files, bro.conf's settings) and the
// file IO of the media and model siblings (bro.mesh, bro.image, broaudio's
// AudioContext, bro.tensor, bro.lm).
//
// Each run is a child bro-headless of an app declaring nothing, with a home,
// a temp directory and a BRO_APP_HOME of its own under one scratch folder, so
// "under home" means a folder made here; grants come from --grant.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');

const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_natives_')));
const home = path.join(scratch, 'home');          // the child's home
const tmp = path.join(scratch, 'tmp');            // its temp directory
const appHome = path.join(scratch, 'apphome');    // its BRO_APP_HOME (and grant store)
const docs = path.join(home, 'Documents');        // the user's files: neither the app's nor temp
for (const d of [home, tmp, appHome, docs]) fs.mkdirSync(d, { recursive: true });
fs.writeFileSync(path.join(docs, 'secret.txt'), 'secret');
// A 1x1 PNG, for <img> and the image readers.
fs.writeFileSync(path.join(docs, 'pixel.png'), Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64'));
fs.writeFileSync(path.join(docs, 'tri.obj'), 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n');

const app = path.join(home, 'apps', 'nativesapp');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.PermNatives', name: 'Natives' }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>natives</body></html>');

let scriptN = 0;
function run(body, args) {
    const script = path.join(scratch, 'script' + (scriptN++) + '.js');
    // What each attempt did: 'ok', 'absent' (compiled out), or the error as
    // "<code>|<message>|<path>".
    const prelude =
        'const path = require("path");\n' +
        'const D = ' + JSON.stringify(docs) + ';\n' +
        'const R = {};\n' +
        'const why = (e) => String(e && e.code) + "|" + String(e && e.message) + "|" + String(e && e.path);\n' +
        'const attempt = (key, fn) => { try { fn(); R[key] = "ok"; } catch (e) { R[key] = why(e); } };\n' +
        'const settle = (key, p) => p.then(() => { R[key] = "ok"; }, (e) => { R[key] = why(e); });\n' +
        'const has = (ns) => !!(ns && ns.available !== false);\n' +
        'const pump = () => { for (let i = 0; i < 40; i++) advanceTime(25); };\n';
    fs.writeFileSync(script, prelude + body + '\nconsole.log("RESULT:" + JSON.stringify(R));\n');
    const env = { ...process.env, BRO_APP_HOME: appHome, HOME: home, USERPROFILE: home, TMP: tmp, TEMP: tmp, TMPDIR: tmp };
    const r = cp.spawnSync(headless, (args || []).concat([app, script]), { env, encoding: 'utf8', timeout: 120000 });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}
// Refused for want of `files`: the embed access error, code EACCES.
const refused = (r) => typeof r === 'string' && r.startsWith('EACCES|') && /files/.test(r);

const probe =
    // bro's own natives: the headless harness's writers, the encoders,
    // bro.media and the screen capture.
    'attempt("writeFile", () => writeFile(path.join(D, "w.txt"), "x"));\n' +
    'attempt("screenshot", () => screenshot(path.join(D, "s.png")));\n' +
    'if (typeof VideoEncoder === "function") attempt("videoEncoder", () => {\n' +
    '    const e = new VideoEncoder({ path: path.join(D, "v.webm"), width: 16, height: 16, fps: 30 }); e.finish(); });\n' +
    'else R.videoEncoder = "absent";\n' +
    'if (typeof GifEncoder === "function") attempt("gifEncoder", () => {\n' +
    '    const e = new GifEncoder({ path: path.join(D, "g.gif"), width: 16, height: 16 }); e.finish(); });\n' +
    'else R.gifEncoder = "absent";\n' +
    'if (has(bro.media) && bro.media.tags) attempt("mediaTags", () => bro.media.tags(path.join(D, "secret.txt")));\n' +
    'else R.mediaTags = "absent";\n' +
    'attempt("captureScreen", () => bro.window.captureScreen(path.join(D, "c.png")));\n' +
    // The desktop siblings.
    'if (has(bro.vfs)) attempt("vfs", () => bro.vfs.getMime(path.join(D, "secret.txt"))); else R.vfs = "absent";\n' +
    'if (has(bro.search)) attempt("search", () => bro.search.filesSync(D)); else R.search = "absent";\n' +
    'if (has(bro.apps)) attempt("appsRecent", () => bro.apps.getRecent()); else R.appsRecent = "absent";\n' +
    'if (has(bro.conf)) settle("conf", bro.conf.set("org.bro.test.natives.flag", "x")); else R.conf = "absent";\n' +
    // The media and model siblings' file IO.
    'if (has(bro.mesh) && bro.mesh.Mesh && bro.mesh.Mesh.loadOBJ) attempt("meshLoad", () => bro.mesh.Mesh.loadOBJ(path.join(D, "tri.obj")));\n' +
    'else R.meshLoad = "absent";\n' +
    'if (has(bro.image) && bro.image.encodePngFile)\n' +
    '    attempt("imageWrite", () => bro.image.encodePngFile(path.join(D, "e.png"), new Uint8Array(4), 1, 1, 4));\n' +
    'else R.imageWrite = "absent";\n' +
    'if (has(bro.lm) && bro.lm.loadTokenizer) attempt("lmLoad", () => bro.lm.loadTokenizer(path.join(D, "tokenizer.json")));\n' +
    'else R.lmLoad = "absent";\n' +
    // A subresource: an <img> naming the file loads or errors.
    'const img = document.createElement("img");\n' +
    'img.onload = () => { R.img = "ok"; }; img.onerror = () => { R.img = "error"; };\n' +
    'img.src = path.join(D, "pixel.png").split(path.sep).join("/"); document.body.appendChild(img);\n' +
    'pump(); flush(); pump();\n' +
    'R.written = ["w.txt", "s.png", "v.webm", "g.gif", "c.png", "e.png"].filter((f) => require("fs").existsSync(path.join(D, f)));\n';

// ---- without `files` ------------------------------------------------------------------
{
    const r = run(probe);
    const keys = ['writeFile', 'screenshot', 'videoEncoder', 'gifEncoder', 'mediaTags', 'captureScreen',
                  'vfs', 'search', 'appsRecent', 'conf', 'meshLoad', 'imageWrite', 'lmLoad'];
    for (const k of keys) {
        if (r[k] === 'absent') { console.log('test_permissions_natives: ' + k + ' compiled out'); continue; }
        assert(refused(r[k]), k + ' refused without files: ' + r[k]);
    }
    // A path refusal names the path, absolute.
    for (const k of ['writeFile', 'vfs', 'search']) {
        if (r[k] === 'absent') continue;
        assert(r[k].split('|')[2].indexOf('Documents') >= 0, k + "'s refusal carries the path: " + r[k]);
    }
    assert(r.img === 'error', 'an <img> naming the file fails to load: ' + r.img);
    assert(r.written.length === 0, 'nothing refused was written: ' + r.written.join(', '));
}

// ---- with `files` ---------------------------------------------------------------------
{
    for (const f of fs.readdirSync(docs)) if (/^(w\.txt|s\.png|v\.webm|g\.gif|c\.png|e\.png)$/.test(f)) fs.rmSync(path.join(docs, f));
    const g = run(probe, ['--grant', 'files']);
    for (const k of ['writeFile', 'screenshot', 'videoEncoder', 'gifEncoder', 'mediaTags', 'vfs', 'search',
                     'appsRecent', 'conf', 'meshLoad', 'imageWrite']) {
        if (g[k] === 'absent') continue;
        assert(g[k] === 'ok', k + ' works with files: ' + g[k]);
    }
    // Granted, these get past the gate and fail, if at all, for their own reasons.
    for (const k of ['captureScreen', 'lmLoad']) {
        if (g[k] === 'absent') continue;
        assert(!refused(g[k]), k + ' is not refused once files is granted: ' + g[k]);
    }
    assert(g.img === 'ok', 'an <img> naming the file loads with files: ' + g.img);
    assert(g.written.indexOf('w.txt') >= 0 && g.written.indexOf('s.png') >= 0, 'the granted writes landed: ' + g.written.join(', '));
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
