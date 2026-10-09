// The app manifest and runtime (docs/apps.md, docs/app-api.js): bro.json's
// identity and desktop keys reach bro.app, the per-app directories are keyed
// by the id, an anonymous app gets its folder's name, privileged namespaces
// stay stubs unless the user grants them, and `bro --desktop-entry` /
// `--install` turn the folder into a FreeDesktop application.
//
// Each case runs the fixture app in a child bro-headless with a scratch
// environment, so nothing here touches the user's own state.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const isWin = process.platform === 'win32';
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');
const exeDir = path.dirname(headless);
const windowed = path.join(exeDir, isWin ? 'bro.exe' : 'bro');

const fixture = path.resolve('tests/app/fixtures/manifest_app');
const plain = path.resolve('tests/app/fixtures/plain_app');
const scratch = path.join(os.tmpdir(), 'bro_app_test_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });

function scratchEnv(extra) {
    const env = { ...process.env };
    delete env.BRO_TRUSTED_APP_DIR;
    delete env.BRO_APP_HOME;
    return { ...env, ...(extra || {}) };
}

// Run `expr` (an expression yielding a JSON-able value) in the app under a
// child bro-headless and answer that value.
function probe(appDir, expr, env, extraArgs) {
    const code = 'console.log("RESULT:" + JSON.stringify(' + expr + '))';
    const args = [appDir, '-e', code].concat(extraArgs || []);
    const out = cp.execFileSync(headless, args, { env: scratchEnv(env), encoding: 'utf8' });
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}

// Real paths: macOS's temp directory is /var/..., a symlink to /private/var/...
const real = (p) => { try { return fs.realpathSync(p); } catch (e) { return path.resolve(p); } };
const norm = (p) => real(p).replace(/\\/g, '/').toLowerCase();

// ---- identity and manifest ----------------------------------------------------
{
    const home = path.join(scratch, 'home1');
    const a = probe(fixture, 'bro.app', { BRO_APP_HOME: home }, ['--', 'one', 'two three', '']);
    assert(a.id === 'org.bro.test.ManifestApp', 'id from bro.json: ' + a.id);
    assert(a.name === 'Manifest App', 'name: ' + a.name);
    assert(a.version === '1.2.3', 'version: ' + a.version);
    assert(a.description.indexOf('reads its own manifest') >= 0, 'description');
    assert(norm(a.dir) === norm(fixture), 'dir is the app folder: ' + a.dir);
    assert(norm(a.icon) === norm(path.join(fixture, 'icon.svg')), 'icon resolved against the app dir: ' + a.icon);
    assert(JSON.stringify(a.argv) === JSON.stringify(['one', 'two three', '']),
        'argv is what followed --, empty argument included: ' + JSON.stringify(a.argv));
    assert(norm(a.cwd) === norm(process.cwd()), 'cwd is where it was launched from: ' + a.cwd);
    assert(a.singleInstance === false, 'a headless run does not claim the instance channel by default');
    assert(typeof a.exePath === 'string' && a.exePath.length > 0, 'exePath');
    const m = a.manifest;
    assert(m.singleInstance === true, 'manifest.singleInstance');
    assert(JSON.stringify(m.categories) === '["Utility","Development"]', 'categories: ' + JSON.stringify(m.categories));
    assert(m.fileTypes.length === 1 && m.fileTypes[0].mimeTypes[0] === 'text/plain', 'fileTypes');
    assert(JSON.stringify(m.fileTypes[0].extensions) === '["txt","text"]', 'extensions lose their dot: ' +
        JSON.stringify(m.fileTypes[0].extensions));
    assert(m.actions.length === 1 && m.actions[0].id === 'new-window' &&
        JSON.stringify(m.actions[0].args) === '["--new-window"]', 'actions (the malformed one skipped): ' +
        JSON.stringify(m.actions));
    assert(JSON.stringify(a.permissions.requested) === '["remote","compositor"]', 'permissions.requested');
    assert(a.permissions.granted.length === 0 && a.permissions.shell === false, 'nothing granted by asking');
}

// ---- per-app directories ------------------------------------------------------
{
    const home = path.join(scratch, 'home2');
    const d = probe(fixture, '({c: bro.app.configDir, d: bro.app.dataDir, k: bro.app.cacheDir})', { BRO_APP_HOME: home });
    assert(norm(d.c) === norm(path.join(home, 'config')), 'BRO_APP_HOME/config: ' + d.c);
    assert(norm(d.d) === norm(path.join(home, 'data')), 'BRO_APP_HOME/data: ' + d.d);
    assert(norm(d.k) === norm(path.join(home, 'cache')), 'BRO_APP_HOME/cache: ' + d.k);
    for (const p of [d.c, d.d, d.k]) assert(fs.existsSync(p), 'created on first read: ' + p);

    // Without BRO_APP_HOME: the platform's directories, keyed by the id.
    const base = path.join(scratch, 'platform');
    const env = isWin
        ? { APPDATA: path.join(base, 'Roaming'), LOCALAPPDATA: path.join(base, 'Local') }
        : { XDG_CONFIG_HOME: path.join(base, 'config'), XDG_DATA_HOME: path.join(base, 'data'),
            XDG_CACHE_HOME: path.join(base, 'cache'), XDG_STATE_HOME: path.join(base, 'state'),
            HOME: base };
    const p = probe(fixture, '({c: bro.app.configDir, d: bro.app.dataDir, k: bro.app.cacheDir})', env);
    const id = 'org.bro.test.ManifestApp';
    if (isWin) {
        assert(norm(p.c) === norm(path.join(base, 'Roaming', id)), 'config is %APPDATA%\\<id>: ' + p.c);
        assert(norm(p.d) === norm(path.join(base, 'Roaming', id, 'data')), 'data: ' + p.d);
        assert(norm(p.k) === norm(path.join(base, 'Local', id, 'cache')), 'cache is %LOCALAPPDATA%\\<id>\\cache: ' + p.k);
    } else if (process.platform === 'linux') {
        assert(norm(p.c) === norm(path.join(base, 'config', id)), 'config is $XDG_CONFIG_HOME/<id>: ' + p.c);
        assert(norm(p.d) === norm(path.join(base, 'data', id)), 'data is $XDG_DATA_HOME/<id>: ' + p.d);
        assert(norm(p.k) === norm(path.join(base, 'cache', id)), 'cache is $XDG_CACHE_HOME/<id>: ' + p.k);
    } else if (process.platform === 'darwin') {
        const support = path.join(base, 'Library', 'Application Support');
        assert(norm(p.c) === norm(path.join(support, id)), 'config is ~/Library/Application Support/<id>: ' + p.c);
        assert(norm(p.d) === norm(path.join(support, id, 'data')), 'data: ' + p.d);
        assert(norm(p.k) === norm(path.join(base, 'Library', 'Caches', id)), 'cache is ~/Library/Caches/<id>: ' + p.k);
    }

    // An app with no manifest is named after its folder.
    const q = probe(plain, '({id: bro.app.id, name: bro.app.name, c: bro.app.configDir})',
        { BRO_APP_HOME: path.join(scratch, 'home3') });
    assert(q.id === 'plain_app' && q.name === 'plain_app', 'the folder name is the id: ' + JSON.stringify(q));
}

// ---- permission gating --------------------------------------------------------
{
    const ns = (name) => '({a: bro.' + name + '.available, r: bro.' + name + '.reason})';
    const home = { BRO_APP_HOME: path.join(scratch, 'home4') };
    for (const name of ['remote', 'compositor']) {
        const r = probe(plain, ns(name), home);
        assert(r.a === false, 'bro.' + name + ' is a stub for an ordinary app');
        assert(typeof r.r === 'string' && r.r.length > 0, 'bro.' + name + ' says why: ' + r.r);
        const msg = probe(plain, '(() => { try { bro.' + name + '.host({}); return "no throw"; } ' +
            'catch (e) { return e.message; } })()', home);
        assert(msg.indexOf(r.r) >= 0, 'a call into bro.' + name + ' throws the reason: ' + msg);
    }
    // The manifest asks; asking alone is refused.
    const asked = probe(fixture, '({remote: ' + ns('remote').slice(1, -1) + ', granted: bro.app.permissions.granted})', home);
    assert(asked.remote.a === false && asked.granted.length === 0, 'asking grants nothing: ' + JSON.stringify(asked));

    // The user's permissions file grants `remote` to this id, and only what
    // the app asked for (compositor is asked for but not granted; sys is
    // granted but not asked for).
    // The file: %APPDATA%\bro, $XDG_CONFIG_HOME/bro, ~/Library/Application Support/bro.
    const cfg = path.join(scratch, 'usercfg');
    const mac = process.platform === 'darwin';
    const permDir = mac ? path.join(cfg, 'Library', 'Application Support', 'bro') : path.join(cfg, 'bro');
    fs.mkdirSync(permDir, { recursive: true });
    fs.writeFileSync(path.join(permDir, 'permissions.json'),
        JSON.stringify({ 'org.bro.test.ManifestApp': ['remote', 'sys'] }));
    const grantEnv = { ...home, ...(isWin ? { APPDATA: cfg } : mac ? { HOME: cfg } : { XDG_CONFIG_HOME: cfg }) };
    const g = probe(fixture, '({remote: bro.remote.available, reason: bro.remote.reason, comp: bro.compositor.available, ' +
        'sys: bro.sys.available, granted: bro.app.permissions.granted})', grantEnv);
    assert(JSON.stringify(g.granted) === '["remote"]', 'the user granted remote: ' + JSON.stringify(g));
    if (g.remote === false) {
        assert(/compiled without/.test(g.reason), 'bro.remote is unavailable only because it is compiled out: ' + g.reason);
    }
    assert(g.comp === false, 'compositor, not granted, stays a stub');
    assert(g.sys === false, 'sys, granted but not asked for, stays a stub');
}

// ---- desktop entry and install ------------------------------------------------
if (!fs.existsSync(windowed)) {
    console.log('test_app_manifest: no windowed bro beside ' + headless + '; --install / --desktop-entry not tested');
} else {
    const entry = cp.execFileSync(windowed, ['--desktop-entry', fixture, '--exec', '/opt/bro/bin/bro'],
        { env: scratchEnv(), encoding: 'utf8' });
    const has = (line) => entry.split(/\r?\n/).indexOf(line) >= 0;
    assert(has('[Desktop Entry]') && has('Type=Application'), 'a desktop entry: ' + entry);
    assert(has('Name=Manifest App'), 'Name');
    assert(has('StartupWMClass=org.bro.test.ManifestApp'), 'StartupWMClass is the id (the window app_id)');
    assert(has('Categories=Utility;Development;'), 'Categories');
    assert(has('MimeType=text/plain;'), 'MimeType from fileTypes');
    assert(has('Actions=new-window;') && has('[Desktop Action new-window]'), 'the action');
    assert(has('SingleMainWindow=true'), 'single instance');
    const exec = entry.split(/\r?\n/).find((l) => l.startsWith('Exec='));
    assert(exec && exec.startsWith('Exec=/opt/bro/bin/bro ') && exec.endsWith(' %F'),
        'Exec runs bro on the app dir and takes files: ' + exec);

    // --install copies the folder to the user's apps root; the id then
    // launches it.
    const data = path.join(scratch, 'install');
    // XDG_DATA_DIRS stays: the Vulkan loader finds its drivers through it.
    // macOS keeps apps in ~/Library/Application Support/bro/apps.
    const isMac = process.platform === 'darwin';
    const env = isWin ? { LOCALAPPDATA: data } : isMac ? { HOME: data } : { XDG_DATA_HOME: data };
    cp.execFileSync(windowed, ['--install', fixture, '--exec', '/opt/bro/bin/bro'], { env: scratchEnv(env), encoding: 'utf8' });
    const installed = isMac ? path.join(data, 'Library', 'Application Support', 'bro', 'apps', 'org.bro.test.ManifestApp')
                            : path.join(data, 'bro', 'apps', 'org.bro.test.ManifestApp');
    assert(fs.existsSync(path.join(installed, 'bro.json')), 'installed at ' + installed);
    if (!isWin && !isMac) {
        const de = path.join(data, 'applications', 'org.bro.test.ManifestApp.desktop');
        assert(fs.existsSync(de), 'desktop entry written to ' + de);
        assert(fs.readFileSync(de, 'utf8').indexOf(installed) >= 0, 'it runs the installed copy');
    }
    const byId = probe('org.bro.test.ManifestApp', '({id: bro.app.id, dir: bro.app.dir})',
        { ...env, BRO_APP_HOME: path.join(scratch, 'home5') });
    assert(byId.id === 'org.bro.test.ManifestApp' && norm(byId.dir) === norm(installed),
        'bro-headless <id> runs the installed app: ' + JSON.stringify(byId));
    const list = cp.execFileSync(windowed, ['--list-apps'], { env: scratchEnv(env), encoding: 'utf8' });
    assert(list.indexOf('org.bro.test.ManifestApp') >= 0, '--list-apps lists it: ' + list);
    cp.execFileSync(windowed, ['--uninstall', 'org.bro.test.ManifestApp'], { env: scratchEnv(env), encoding: 'utf8' });
    assert(!fs.existsSync(installed), '--uninstall removed it');
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
console.log('test_app_manifest.js PASSED');
