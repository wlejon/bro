// The filesystem an app reaches (docs/apps.md, Permissions): without `files`
// it writes only its own directories, the OS temp directory and what the
// user picked, and reads anywhere outside the user's home but, under it,
// only those, its own folder and bro's. `..` and links are resolved before
// deciding; a picked file or folder stays the app's on later runs; nothing
// writes bro's grant store.
//
// The child runs with a home, a temp directory and a BRO_APP_HOME of its own
// under one scratch folder, so "under home" means a folder made here.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');

const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_fs_')));
const home = path.join(scratch, 'home');          // the child's home
const tmp = path.join(scratch, 'tmp');            // its temp directory
const appHome = path.join(scratch, 'apphome');    // its BRO_APP_HOME (and grant store)
const outside = path.join(scratch, 'outside');    // neither home nor temp
for (const d of [home, tmp, appHome, outside]) fs.mkdirSync(d, { recursive: true });
fs.writeFileSync(path.join(home, 'secret.txt'), 'secret');
fs.writeFileSync(path.join(outside, 'open.txt'), 'open');

// The app's folder is under the home: its own files are readable anyway.
const app = path.join(home, 'apps', 'fsapp');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.PermFs', name: 'Fs', permissions: ['files'] }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>fs</body></html>');
fs.writeFileSync(path.join(app, 'data.txt'), 'mine');

const P = { home, tmp, appHome, outside, app };

let scriptN = 0;
function run(body, args) {
    const script = path.join(scratch, 'script' + (scriptN++) + '.js');
    // What each attempt did: 'ok' or the error's message.
    const prelude =
        'const fs = require("fs"); const path = require("path");\n' +
        'const P = ' + JSON.stringify(P) + ';\n' +
        'const R = {};\n' +
        'const attempt = (key, fn) => { try { fn(); R[key] = "ok"; } catch (e) { R[key] = String(e.message); } };\n';
    fs.writeFileSync(script, prelude + body + '\nconsole.log("RESULT:" + JSON.stringify(R));\n');
    const env = { ...process.env, BRO_APP_HOME: appHome, HOME: home, USERPROFILE: home, TMP: tmp, TEMP: tmp, TMPDIR: tmp };
    const r = cp.spawnSync(headless, (args || []).concat([app, script]), { env, encoding: 'utf8', timeout: 90000 });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}
const refusedFiles = (msg) => typeof msg === 'string' && msg !== 'ok' && /files/.test(msg);

// ---- without `files` ---------------------------------------------------------------
{
    const r = run(
        'attempt("writeData", () => fs.writeFileSync(path.join(bro.app.dataDir, "a.txt"), "x"));\n' +
        'attempt("writeConfig", () => fs.mkdirSync(path.join(bro.app.configDir, "sub"), { recursive: true }));\n' +
        'attempt("writeTemp", () => fs.writeFileSync(path.join(P.tmp, "t.txt"), "x"));\n' +
        'attempt("writeOutside", () => fs.writeFileSync(path.join(P.outside, "w.txt"), "x"));\n' +
        'attempt("writeHome", () => fs.writeFileSync(path.join(P.home, "w.txt"), "x"));\n' +
        'attempt("appendOutside", () => fs.appendFileSync(path.join(P.outside, "open.txt"), "x"));\n' +
        'attempt("openWriteOutside", () => fs.closeSync(fs.openSync(path.join(P.outside, "fd.txt"), "w")));\n' +
        'attempt("renameOut", () => { fs.writeFileSync(path.join(P.tmp, "m.txt"), "x"); fs.renameSync(path.join(P.tmp, "m.txt"), path.join(P.outside, "m.txt")); });\n' +
        'attempt("readOutside", () => fs.readFileSync(path.join(P.outside, "open.txt"), "utf8"));\n' +
        'attempt("readApp", () => fs.readFileSync(path.join(P.app, "data.txt"), "utf8"));\n' +
        'attempt("readSecret", () => fs.readFileSync(path.join(P.home, "secret.txt"), "utf8"));\n' +
        'attempt("listHome", () => fs.readdirSync(P.home));\n' +
        'attempt("statSecret", () => fs.statSync(path.join(P.home, "secret.txt")));\n' +
        'attempt("dotdotRead", () => fs.readFileSync(path.join(P.app, "..", "..", "secret.txt"), "utf8"));\n' +
        'attempt("dotdotWrite", () => fs.writeFileSync(path.join(bro.app.dataDir, "..", "..", "..", "outside", "d.txt"), "x"));\n' +
        'attempt("grantStore", () => fs.writeFileSync(path.join(P.appHome, "bro", "grants.json"), "{}"));\n' +
        'fs.promises.writeFile(path.join(P.outside, "async.txt"), "x").then(() => { R.asyncOutside = "ok"; }, (e) => { R.asyncOutside = String(e.message); });\n' +
        'advanceTime(16); advanceTime(16);\n');
    for (const k of ['writeData', 'writeConfig', 'writeTemp', 'readOutside', 'readApp']) {
        assert(r[k] === 'ok', k + ' is allowed without files: ' + r[k]);
    }
    for (const k of ['writeOutside', 'writeHome', 'appendOutside', 'openWriteOutside', 'renameOut', 'readSecret',
                     'listHome', 'statSecret', 'dotdotRead', 'dotdotWrite', 'asyncOutside']) {
        assert(refusedFiles(r[k]), k + ' is refused, naming files: ' + r[k]);
    }
    assert(r.grantStore !== 'ok', 'the grant store is not writable: ' + r.grantStore);
    assert(!fs.existsSync(path.join(outside, 'w.txt')) && !fs.existsSync(path.join(outside, 'async.txt')),
        'nothing was written outside');
    assert(r.readSecret.indexOf('secret.txt') >= 0, 'the refusal names the path: ' + r.readSecret);
}

// ---- links out of an allowed directory --------------------------------------------
{
    // Made here (this run has `files`): a link in the app's data directory
    // to the home and one to the outside folder.
    const data = path.join(appHome, 'data');
    fs.mkdirSync(data, { recursive: true });
    let linked = true;
    try {
        fs.symlinkSync(home, path.join(data, 'tohome'), 'junction');
        fs.symlinkSync(outside, path.join(data, 'toout'), 'junction');
    } catch (e) {
        linked = false;
        console.log('test_permissions_fs: links not made (' + e.message + '); link escapes not tested');
    }
    if (linked) {
        const r = run(
            'attempt("readThrough", () => fs.readFileSync(path.join(bro.app.dataDir, "tohome", "secret.txt"), "utf8"));\n' +
            'attempt("writeThrough", () => fs.writeFileSync(path.join(bro.app.dataDir, "toout", "l.txt"), "x"));\n');
        assert(refusedFiles(r.readThrough), 'reading home through a link is refused: ' + r.readThrough);
        assert(refusedFiles(r.writeThrough), 'writing outside through a link is refused: ' + r.writeThrough);
        assert(!fs.existsSync(path.join(outside, 'l.txt')), 'nothing was written through the link');
    }
}

// ---- picked by the user -------------------------------------------------------------
{
    const pickedFile = path.join(home, 'picked.txt');
    const pickedDir = path.join(home, 'pickdir');
    const savedFile = path.join(outside, 'saved.txt');
    fs.writeFileSync(pickedFile, 'p');
    fs.mkdirSync(pickedDir, { recursive: true });
    const r = run(
        'setPickedFiles([' + JSON.stringify(pickedFile) + ']); showOpenFileDialog();\n' +
        'attempt("file", () => fs.writeFileSync(' + JSON.stringify(pickedFile) + ', "written"));\n' +
        'attempt("beside", () => fs.writeFileSync(path.join(P.home, "beside.txt"), "x"));\n' +
        'setPickedFiles([' + JSON.stringify(pickedDir) + ']); showOpenFolderDialog();\n' +
        'attempt("inDir", () => { fs.mkdirSync(path.join(' + JSON.stringify(pickedDir) + ', "sub"), { recursive: true }); ' +
        'fs.writeFileSync(path.join(' + JSON.stringify(pickedDir) + ', "sub", "n.txt"), "x"); });\n' +
        'setPickedFiles([' + JSON.stringify(savedFile) + ']); showSaveFileDialog("Text|txt", "saved.txt");\n' +
        'attempt("saved", () => fs.writeFileSync(' + JSON.stringify(savedFile) + ', "x"));\n');
    assert(r.file === 'ok', 'a picked file is writable: ' + r.file);
    assert(refusedFiles(r.beside), 'its neighbours are not: ' + r.beside);
    assert(r.inDir === 'ok', 'a picked folder is writable throughout: ' + r.inDir);
    assert(r.saved === 'ok', 'a path chosen to save to is writable: ' + r.saved);

    // The next run picks nothing: what was picked is still the app's.
    const again = run(
        'attempt("file", () => fs.readFileSync(' + JSON.stringify(pickedFile) + ', "utf8"));\n' +
        'attempt("inDir", () => fs.writeFileSync(path.join(' + JSON.stringify(pickedDir) + ', "again.txt"), "x"));\n');
    assert(again.file === 'ok' && again.inDir === 'ok', 'picks persist across runs: ' + JSON.stringify(again));
    const store = fs.readFileSync(path.join(appHome, 'bro', 'grants.json'), 'utf8');
    assert(store.indexOf('pickdir') >= 0, 'the store holds the picks: ' + store);
}

// ---- with `files` --------------------------------------------------------------------
{
    const r = run(
        'attempt("readSecret", () => fs.readFileSync(path.join(P.home, "secret.txt"), "utf8"));\n' +
        'attempt("writeOutside", () => fs.writeFileSync(path.join(P.outside, "g.txt"), "x"));\n' +
        'attempt("grantStore", () => fs.writeFileSync(path.join(P.appHome, "bro", "grants.json"), "{}"));\n',
        ['--grant', 'files']);
    assert(r.readSecret === 'ok' && r.writeOutside === 'ok', 'files reads and writes the rest: ' + JSON.stringify(r));
    assert(r.grantStore !== 'ok', 'but never bro\'s grant store: ' + r.grantStore);
    assert(JSON.parse(fs.readFileSync(path.join(appHome, 'bro', 'grants.json'), 'utf8')).apps,
        'the grant store is intact');
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
