// A preview bro.thumb hands an app is the app's to read (AccessKind::ReadDerived):
// the shared thumbnail cache is in the user's home, which an app without
// `files` may not read, so an <img src> of a preview of a photo it may read
// (on another drive, in its own folder) would stay blank. Before the result
// the cached file is refused like any other in home; after it, the app reads
// it and an <img> of it loads, with nothing granted.
//
// A child bro-headless of an app declaring nothing, in a scratch folder under
// temp (which an app may read), with the real home, so the cache is where it
// is for the user.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

if (!bro.thumb || bro.thumb.available === false) {
    skipTest('bro.thumb is compiled out of this build');
} else {
    const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
    assert(headless, 'a bro-headless binary to run the fixture');
    const home = os.homedir();
    const cacheDir = bro.thumb.getBaseDir();
    const under = (p, root) => path.relative(root, p).split(path.sep)[0] !== '..' && !path.isAbsolute(path.relative(root, p));
    if (!under(path.resolve(cacheDir), path.resolve(home))) {
        skipTest('the thumbnail cache (' + cacheDir + ') is outside home here, so nothing gates it');
    } else {
        const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_thumb_derived_')));
        const app = path.join(scratch, 'app');
        // The child's home, with the user folders in it where the OS puts
        // them: the thumbnail cache beside the app's own folders, not in them
        // (no BRO_APP_HOME, which puts the cache inside the app's cache
        // folder; the grant store goes to the scratch APPDATA).
        const childHome = path.join(scratch, 'home');
        const appData = path.join(childHome, 'AppData');
        fs.mkdirSync(app, { recursive: true });
        fs.mkdirSync(appData, { recursive: true });
        fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.ThumbDerived', name: 'Derived' }));
        fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body></body></html>');
        // A 64x64 photo-sized-enough PNG, its name unique so the cache has no
        // preview of it from another run.
        const src = path.join(app, 'photo-' + process.pid + '-' + Date.now() + '.png');
        const px = new Uint8Array(64 * 64 * 4);
        for (let i = 0; i < px.length; i += 4) { px[i] = (i / 4) % 64 * 4; px[i + 1] = 120; px[i + 2] = 200; px[i + 3] = 255; }
        bro.image.encodePngFile(src, px, 64, 64, 4);

        const script = path.join(scratch, 'probe.js');
        fs.writeFileSync(script,
            'const fs = require("fs");\n' +
            'const SRC = ' + JSON.stringify(src) + ';\n' +
            'const R = {};\n' +
            'const why = (e) => String(e && e.code) + "|" + String(e && e.message);\n' +
            'const read = (p) => { try { fs.readFileSync(p); return "ok"; } catch (e) { return why(e); } };\n' +
            'R.before = read(bro.thumb.getThumbnailPath(SRC, { size: "large" }));\n' +
            'let res = null, err = null;\n' +
            'try { res = bro.thumb.getSync(SRC, { size: "large" }); } catch (e) { err = why(e); }\n' +
            'R.got = res ? "ok" : String(err);\n' +
            'if (res) {\n' +
            '  R.path = res.path;\n' +
            '  R.after = read(res.path);\n' +
            '  const img = document.createElement("img");\n' +
            '  img.onload = () => { R.img = "ok"; }; img.onerror = () => { R.img = "error"; };\n' +
            '  img.src = res.path.split("\\\\").join("/"); document.body.appendChild(img);\n' +
            '  for (let i = 0; i < 100 && !R.img; i++) { advanceTime(10); flush(); sleep(5); }\n' +
            '}\n' +
            'console.log("RESULT:" + JSON.stringify(R));\n');
        // Its temp directory (any app's to read) a folder beside the home, not round it.
        const tmp = path.join(scratch, 'tmp');
        fs.mkdirSync(tmp, { recursive: true });
        const env = { ...process.env, HOME: childHome, USERPROFILE: childHome,
                      APPDATA: path.join(appData, 'Roaming'), LOCALAPPDATA: path.join(appData, 'Local'),
                      XDG_CACHE_HOME: path.join(childHome, '.cache'), XDG_CONFIG_HOME: path.join(childHome, '.config'),
                      XDG_DATA_HOME: path.join(childHome, '.local', 'share'), TMP: tmp, TEMP: tmp, TMPDIR: tmp };
        delete env.BRO_APP_HOME;
        const r = cp.spawnSync(headless, [app, script], { env, encoding: 'utf8', timeout: 120000 });
        const out = (r.stdout || '') + (r.stderr || '');
        const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
        assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
        const R = JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
        assert(/^EACCES\|/.test(R.before) && /files/.test(R.before),
               'the cached preview is refused before bro.thumb hands it over: ' + R.before);
        assert(R.got === 'ok', 'bro.thumb.get makes the preview: ' + R.got);
        assert(R.after === 'ok', 'the app reads the preview it was handed: ' + R.after + ' (' + R.path + ')');
        assert(R.img === 'ok', 'an <img> of the preview loads: ' + R.img);
        try { bro.thumb.invalidate(src); } catch (_) { /* not cached */ }
        fs.rmSync(scratch, { recursive: true, force: true });
        console.log('test_thumb_derived_read OK');
    }
}
