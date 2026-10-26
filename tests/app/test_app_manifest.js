// The app manifest and runtime (docs/apps.md, docs/app-api.js): bro.json's
// identity and desktop keys reach bro.app, the per-app directories are keyed
// by the id, an anonymous app gets its folder's name, gated namespaces stay
// stubs until the app is granted their capability, and `bro --desktop-entry` /
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
    // Each case names its own BRO_APP_HOME, which also holds bro's grant
    // store: never the user's own grants.
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
    assert(JSON.stringify(a.permissions.declared) === '["remote","compositor"]', 'permissions.declared');
    assert(a.permissions.granted.length === 0, 'nothing granted by declaring: ' + JSON.stringify(a.permissions));
}

// ---- per-app directories ------------------------------------------------------
{
    const home = path.join(scratch, 'home2');
    const d = probe(fixture, '({c: bro.app.configDir, d: bro.app.dataDir, k: bro.app.cacheDir})', { BRO_APP_HOME: home });
    assert(norm(d.c) === norm(path.join(home, 'config')), 'BRO_APP_HOME/config: ' + d.c);
    assert(norm(d.d) === norm(path.join(home, 'data')), 'BRO_APP_HOME/data: ' + d.d);
    assert(norm(d.k) === norm(path.join(home, 'cache')), 'BRO_APP_HOME/cache: ' + d.k);
    for (const p of [d.c, d.d, d.k]) assert(fs.existsSync(p), 'created on first read: ' + p);

    // A BRO_APP_HOME ending in a separator, with a doubled one and a '..' in
    // it (macOS's TMPDIR ends in '/'): the directories are in normal form, so
    // a path joined under one (path.join normalizes) starts with it.
    const sep = isWin ? '\\' : '/';
    const odd = scratch + sep + sep + 'x' + sep + '..' + sep + 'home3' + sep;
    const n = probe(fixture, '({c: bro.app.configDir, d: bro.app.dataDir, k: bro.app.cacheDir, l: bro.app.logFile})',
                    { BRO_APP_HOME: odd });
    const clean = path.join(scratch, 'home3');
    for (const [got, sub] of [[n.c, 'config'], [n.d, 'data'], [n.k, 'cache']]) {
        assert(got === path.join(clean, sub), `BRO_APP_HOME '${odd}': ${sub} is ${path.join(clean, sub)}, got ${got}`);
        assert(path.join(got, 'f.json').startsWith(got), `a file joined under ${got} starts with it`);
    }
    // (A headless run keeps no log file of its own: logFile is '' then.)
    assert(!n.l || path.dirname(n.l) === path.join(clean, 'log'), 'and the log under it: ' + n.l);

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
    // The manifest declares remote, which is then the user's to decide (a
    // headless run nobody answers decides nothing), and compositor, which
    // only a host grants.
    const asked = probe(fixture, '({remote: ' + ns('remote').slice(1, -1) + ', p: bro.app.permissions})', home);
    assert(asked.remote.a === false && asked.p.granted.length === 0, 'declaring grants nothing: ' + JSON.stringify(asked));
    assert(asked.p.undecided.indexOf('remote') >= 0, 'remote is undecided: ' + JSON.stringify(asked.p));
    assert(asked.p.denied.indexOf('compositor') >= 0, 'compositor is denied: ' + JSON.stringify(asked.p));

    // The host grants remote (tests/permissions/ covers the user's grants).
    const g = probe(fixture, '({remote: bro.remote.available, reason: bro.remote.reason, comp: bro.compositor.available, ' +
        'granted: bro.app.permissions.granted})', home, ['--grant', 'remote']);
    assert(JSON.stringify(g.granted) === '["remote"]', 'the host granted remote: ' + JSON.stringify(g));
    if (g.remote === false) {
        assert(/compiled without/.test(g.reason), 'bro.remote is unavailable only because it is compiled out: ' + g.reason);
    }
    assert(g.comp === false, 'compositor, not granted, stays a stub');
}

// ---- an app inside a project ---------------------------------------------------
// Launched by its own folder, the app walks up to the project and mounts the
// library the project's bro.json names ("lib": "shared") at /lib.
const member = path.resolve('tests/app/fixtures/project/member');
{
    const r = probe(member, '({id: bro.app.id, greeting: globalThis.sharedGreeting})',
        { BRO_APP_HOME: path.join(scratch, 'home-member') });
    assert(r.id === 'org.bro.test.ProjectMember', 'the member keeps its own id: ' + r.id);
    assert(r.greeting === 'from the project library', '/lib is the project\'s "shared" folder: ' + JSON.stringify(r));
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
    // APPDATA too: on Windows --install writes the app's Start menu shortcut.
    // These name the user's own roots, so the runs below take no
    // BRO_APP_HOME (a scratch profile has apps of its own, docs/apps.md): the
    // platform's config, cache and state homes go under `data` instead.
    const env = isWin ? { LOCALAPPDATA: data, APPDATA: path.join(data, 'Roaming') }
              : isMac ? { HOME: data }
              : { XDG_DATA_HOME: data, XDG_CONFIG_HOME: path.join(data, 'config'),
                  XDG_CACHE_HOME: path.join(data, 'cache'), XDG_STATE_HOME: path.join(data, 'state') };
    cp.execFileSync(windowed, ['--install', fixture, '--exec', '/opt/bro/bin/bro'], { env: scratchEnv(env), encoding: 'utf8' });
    const installed = isMac ? path.join(data, 'Library', 'Application Support', 'bro', 'apps', 'org.bro.test.ManifestApp')
                            : path.join(data, 'bro', 'apps', 'org.bro.test.ManifestApp');
    assert(fs.existsSync(path.join(installed, 'bro.json')), 'installed at ' + installed);
    if (!isWin && !isMac) {
        const de = path.join(data, 'applications', 'org.bro.test.ManifestApp.desktop');
        assert(fs.existsSync(de), 'desktop entry written to ' + de);
        assert(fs.readFileSync(de, 'utf8').indexOf(installed) >= 0, 'it runs the installed copy');
    }
    const byId = probe('org.bro.test.ManifestApp', '({id: bro.app.id, dir: bro.app.dir})', env);
    assert(byId.id === 'org.bro.test.ManifestApp' && norm(byId.dir) === norm(installed),
        'bro-headless <id> runs the installed app: ' + JSON.stringify(byId));
    const list = cp.execFileSync(windowed, ['--list-apps'], { env: scratchEnv(env), encoding: 'utf8' });
    assert(list.indexOf('org.bro.test.ManifestApp') >= 0, '--list-apps lists it: ' + list);
    cp.execFileSync(windowed, ['--uninstall', 'org.bro.test.ManifestApp'], { env: scratchEnv(env), encoding: 'utf8' });
    assert(!fs.existsSync(installed), '--uninstall removed it');

    // A copy of a project's app carries the project library as its own lib/,
    // so it runs with no project around it.
    const appsRoot = path.dirname(installed);
    cp.execFileSync(windowed, ['--install', member], { env: scratchEnv(env), encoding: 'utf8' });
    const copied = path.join(appsRoot, 'org.bro.test.ProjectMember');
    assert(fs.existsSync(path.join(copied, 'lib', 'greet.js')), 'the project library bundled at ' + copied + '/lib');
    const inst = probe('org.bro.test.ProjectMember', '({dir: bro.app.dir, greeting: globalThis.sharedGreeting})', env);
    assert(norm(inst.dir) === norm(copied) && inst.greeting === 'from the project library',
        'the installed copy imports its bundled /lib: ' + JSON.stringify(inst));
    cp.execFileSync(windowed, ['--uninstall', 'org.bro.test.ProjectMember'], { env: scratchEnv(env), encoding: 'utf8' });

    // A linked install follows the source, project included. (Windows needs
    // developer mode for a symlink; there it is not tested.)
    if (!isWin) {
        cp.execFileSync(windowed, ['--install', member, '--link'], { env: scratchEnv(env), encoding: 'utf8' });
        const linked = probe('org.bro.test.ProjectMember', '({greeting: globalThis.sharedGreeting})', env);
        assert(linked.greeting === 'from the project library', 'a linked install finds its project: ' + JSON.stringify(linked));
        cp.execFileSync(windowed, ['--uninstall', 'org.bro.test.ProjectMember'], { env: scratchEnv(env), encoding: 'utf8' });
    }
}

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
