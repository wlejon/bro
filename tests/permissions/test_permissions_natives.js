// The natives that take a path of their own (docs/apps.md, Permissions): every
// library's natives ask the one access gate (bronze's embed/embed_access.h),
// so a path in the user's home outside the app's own folders is refused
// without `files` and reached with it, whichever library the native is in:
// bro's own (VideoEncoder, GifEncoder,
// bro.media, bro.window.captureScreen, a notification's and the tray's icon,
// <img>), the desktop siblings (bro.vfs,
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
const pixelB64 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';
fs.writeFileSync(path.join(docs, 'pixel.png'), Buffer.from(pixelB64, 'base64'));
fs.writeFileSync(path.join(docs, 'tri.obj'), 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n');

const app = path.join(home, 'apps', 'nativesapp');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.PermNatives', name: 'Natives' }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>natives</body></html>');

let scriptN = 0;
function run(body, args, extraEnv) {
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
    const env = { ...process.env, BRO_APP_HOME: appHome, HOME: home, USERPROFILE: home, TMP: tmp, TEMP: tmp, TMPDIR: tmp,
                  ...(extraEnv || {}) };
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
    // A notification's and the tray's icon are files the platform reads.
    'attempt("notifyIcon", () => bro.window.notify("t", "b", { icon: path.join(D, "pixel.png") }));\n' +
    'attempt("trayIcon", () => { bro.window.setTray({ icon: path.join(D, "pixel.png") }); bro.window.removeTray(); });\n' +
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
    const keys = ['videoEncoder', 'gifEncoder', 'mediaTags', 'captureScreen',
                  'notifyIcon', 'trayIcon', 'vfs', 'search', 'appsRecent', 'conf', 'meshLoad', 'imageWrite', 'lmLoad'];
    for (const k of keys) {
        if (r[k] === 'absent') { console.log('test_permissions_natives: ' + k + ' compiled out'); continue; }
        assert(refused(r[k]), k + ' refused without files: ' + r[k]);
    }
    // The headless driver's writers are the harness's, not the app's
    // (host_access.h, requireHarnessPathAccess): they write where the script
    // says whatever the app was granted.
    for (const k of ['writeFile', 'screenshot']) assert(r[k] === 'ok', k + ' is the harness\'s: ' + r[k]);
    // A path refusal names the path, absolute.
    for (const k of ['notifyIcon', 'vfs', 'search']) {
        if (r[k] === 'absent') continue;
        assert(r[k].split('|')[2].indexOf('Documents') >= 0, k + "'s refusal carries the path: " + r[k]);
    }
    assert(r.img === 'error', 'an <img> naming the file fails to load: ' + r.img);
    // (The app's own fs cannot see Documents without files, so look from here.)
    const landed = ['w.txt', 's.png', 'v.webm', 'g.gif', 'c.png', 'e.png'].filter((f) => fs.existsSync(path.join(docs, f)));
    assert(landed.join(',') === 'w.txt,s.png', 'only the harness\'s writes landed: ' + landed.join(', '));
}

// ---- with `files` ---------------------------------------------------------------------
{
    for (const f of fs.readdirSync(docs)) if (/^(w\.txt|s\.png|v\.webm|g\.gif|c\.png|e\.png)$/.test(f)) fs.rmSync(path.join(docs, f));
    const g = run(probe, ['--grant', 'files']);
    for (const k of ['writeFile', 'screenshot', 'videoEncoder', 'gifEncoder', 'mediaTags', 'notifyIcon', 'trayIcon',
                     'vfs', 'search', 'appsRecent', 'conf', 'meshLoad', 'imageWrite']) {
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

// ---- an <img> over http asks `net` ----------------------------------------------------
// The page's subresources from the network (<img>, CSS url(), @font-face,
// @import, <video>, scripts and sheets over http) ask `net` for the URL,
// answered without a prompt. A server in a process of its own (granted net)
// serves the PNG and logs every request, so a refused load is seen never to
// have reached it.
{
    const pngB64 = pixelB64;
    const portFile = path.join(scratch, 'http.port');
    const hitsFile = path.join(scratch, 'http.hits');
    const stopFile = path.join(scratch, 'http.stop');
    const serverApp = path.join(scratch, 'serverapp');
    fs.mkdirSync(serverApp, { recursive: true });
    fs.writeFileSync(path.join(serverApp, 'bro.json'), JSON.stringify({ id: 'org.bro.test.PermNativesServer', name: 'Server' }));
    fs.writeFileSync(path.join(serverApp, 'index.html'), '<!DOCTYPE html><html><body>server</body></html>');
    const serverScript = path.join(scratch, 'server.js');
    fs.writeFileSync(serverScript,
        'const fs = require("fs");\n' +
        'const png = Buffer.from(' + JSON.stringify(pngB64) + ', "base64");\n' +
        'const srv = require("http").createServer((req, res) => {\n' +
        '    fs.appendFileSync(' + JSON.stringify(hitsFile) + ', req.url + "\\n");\n' +
        '    res.writeHead(200, { "Content-Type": "image/png", "Content-Length": png.length }); res.end(png); });\n' +
        'srv.listen(0, "127.0.0.1");\n' +
        'fs.writeFileSync(' + JSON.stringify(portFile) + ', String(srv.address().port));\n' +
        'const end = perf.now() + 120000;\n' +
        'while (!fs.existsSync(' + JSON.stringify(stopFile) + ') && perf.now() < end) { wallSleep(10); advanceTime(1); }\n' +
        'srv.close();\n');
    const server = cp.spawn(headless, ['--grant', 'net,files', serverApp, serverScript],
                            { env: { ...process.env, BRO_APP_HOME: appHome }, stdio: ['ignore', 'pipe', 'pipe'] });
    let serverOut = '';
    server.stdout.on('data', (d) => { serverOut = (serverOut + d).slice(-4000); });
    server.stderr.on('data', (d) => { serverOut = (serverOut + d).slice(-4000); });
    const until = perf.now() + 60000;
    let serverExit = null;
    server.on('exit', (code) => { serverExit = code; });
    while (!fs.existsSync(portFile) && serverExit === null && perf.now() < until) { wallSleep(50); advanceTime(1); }
    assert(serverExit === null, 'the http server is running, exit ' + serverExit + ': ' + serverOut);
    assert(fs.existsSync(portFile), 'the http server started');
    const port = Number(fs.readFileSync(portFile, 'utf8'));
    const hits = () => (fs.existsSync(hitsFile) ? fs.readFileSync(hitsFile, 'utf8').split('\n').filter(Boolean) : []);

    // Its own user-data dir, so the remote cache the document loader keeps is
    // the scratch folder's, not the machine's.
    const appData = path.join(scratch, 'appdata');
    fs.mkdirSync(appData, { recursive: true });
    const httpEnv = { APPDATA: appData, XDG_DATA_HOME: appData };
    const httpProbe = (name) =>
        'const himg = document.createElement("img");\n' +
        'himg.onload = () => { R.httpImg = "ok"; }; himg.onerror = () => { R.httpImg = "error"; };\n' +
        'himg.src = "http://127.0.0.1:' + port + '/' + name + '.png"; document.body.appendChild(himg);\n' +
        'for (let i = 0; i < 200 && !R.httpImg; i++) { wallSleep(10); advanceTime(10); flush(); }\n' +
        'R.width = himg.naturalWidth;\n';

    const r = run(httpProbe('refused'), [], httpEnv);
    assert(r.httpImg === 'error', 'an http <img> fails to load without net: ' + r.httpImg);
    assert(hits().indexOf('/refused.png') < 0, 'the refused <img> never reached the server: ' + hits().join(', '));

    const g = run(httpProbe('granted'), ['--grant', 'net'], httpEnv);
    assert(g.httpImg === 'ok' && g.width === 1, 'an http <img> loads with net: ' + g.httpImg + ' ' + g.width);
    assert(hits().indexOf('/granted.png') >= 0, 'the granted <img> was fetched: ' + hits().join(', '));

    fs.writeFileSync(stopFile, '');
    const stopBy = perf.now() + 10000;
    while (server.exitCode === null && perf.now() < stopBy) { wallSleep(20); advanceTime(1); }
    if (server.exitCode === null) { try { server.kill(); } catch (e) { /* best effort */ } }
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
