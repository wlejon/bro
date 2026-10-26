// An installed app's own executable (docs/apps.md, "The app's executable"):
// `bro --build-exe` copies bro-app-stub, stamps the app's identity into it
// (and on Windows its icon and version resources), and the result runs the
// app in its own process, as itself, on the shared engine library. One that
// is out of date (the app's bro.json changed, or it was built against another
// engine) still starts: it runs the folder under the stock bro and rebuilds
// itself in the background. `--install` builds one and points the desktop
// entry at it.
//
// The executables run windowed (SDL's offscreen driver), as the windowed
// self-tests do, against a private copy of the fixture in a scratch dir.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const isWin = process.platform === 'win32';
const isMac = process.platform === 'darwin';
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');
const binDir = path.dirname(headless);
const windowed = path.join(binDir, isWin ? 'bro.exe' : 'bro');
const stub = path.join(binDir, isWin ? 'bro-app-stub.exe' : 'bro-app-stub');

if (!fs.existsSync(windowed) || !fs.existsSync(stub)) {
    skipTest('no windowed bro / bro-app-stub beside ' + headless);
} else if (isMac) {
    skipTest('macOS apps are bundles; app executables are not built there yet');
} else if (process.env.BRO_TEST_ALLOW_RASTER === '1') {
    skipTest('no GPU: the executable runs the windowed bro');
} else {
    run();
}

function run() {
    const scratch = path.join(os.tmpdir(), 'bro_exe_test_' + process.pid + '_' + Date.now());
    const app = path.join(scratch, 'exe_app');
    fs.mkdirSync(app, { recursive: true });
    const src = path.resolve('tests/app/fixtures/exe_app');
    for (const f of fs.readdirSync(src)) fs.copyFileSync(path.join(src, f), path.join(app, f));

    const real = (p) => { try { return fs.realpathSync(p); } catch (e) { return path.resolve(p); } };
    const norm = (p) => real(p).replace(/\\/g, '/').toLowerCase();

    const baseEnv = { ...process.env };
    // Also where bro's grant store is: never the user's own grants.
    baseEnv.BRO_APP_HOME = path.join(scratch, 'home');
    baseEnv.SDL_VIDEODRIVER = 'offscreen';
    baseEnv.SDL_AUDIODRIVER = 'dummy';

    const broCli = (args, extra) =>
        cp.execFileSync(windowed, args, { env: { ...baseEnv, ...(extra || {}) }, encoding: 'utf8', cwd: scratch });
    const info = (exe, extra) => {
        const out = broCli(['--exe-info', exe], extra);
        const kv = {};
        for (const line of out.split(/\r?\n/)) {
            const eq = line.indexOf('=');
            if (eq > 0) kv[line.slice(0, eq)] = line.slice(eq + 1);
        }
        return kv;
    };
    let launches = 0;
    // Run the executable; answer what the page reported.
    const launch = (exe) => {
        const out = path.join(scratch, 'report' + (++launches) + '.json');
        const r = cp.spawnSync(exe, ['--no-splash', 'arg one'],
            { env: { ...baseEnv, BRO_TEST_EXE_OUT: out }, cwd: scratch, timeout: 90000, encoding: 'utf8' });
        assert(r.status === 0, exe + ' exited ' + r.status + (r.error ? ' (' + r.error.message + ')' : '') +
            ': ' + String(r.stderr || '').slice(-2000));
        assert(fs.existsSync(out), 'the page reported to ' + out + '; stderr: ' + String(r.stderr || '').slice(-2000));
        // What the run logged: a windowed app's log goes to its log file
        // (Windows has no console for it), the stub's own notes to stderr.
        let appLog = '';
        try { appLog = fs.readFileSync(path.join(baseEnv.BRO_APP_HOME, 'log', 'org.bro.test.ExeApp.log'), 'utf8'); } catch (e) { /* none */ }
        return { report: JSON.parse(fs.readFileSync(out, 'utf8')), stderr: String(r.stderr || '') + appLog };
    };
    const readLog = (e) => { try { return fs.readFileSync(path.join(aotDirOf(e), 'build.log'), 'utf8').slice(-3000); } catch (err) { return ''; } };
    const until = (pred, ms, what) => {
        const end = Date.now() + ms;
        while (Date.now() < end && !pred()) wallSleep(100);
        assert(pred(), 'timed out: ' + what);
    };

    const exe = path.join(scratch, 'bin', isWin ? 'org-bro-test-ExeApp.exe' : 'org-bro-test-ExeApp');
    const aotDirOf = (e) => e.replace(/\.exe$/i, '') + '.aot';

    // ---- build ------------------------------------------------------------------
    broCli(['--build-exe', app, '--out', exe]);
    assert(fs.existsSync(exe), 'built ' + exe);
    let id = info(exe);
    // The page's script, compiled ahead of time beside the executable.
    assert(norm(id['aot.dir']) === norm(aotDirOf(exe)), 'the AOT directory is beside it: ' + JSON.stringify(id));
    assert(id['aot.units'] === '1' && id['aot.built'] === '1' && / built /.test(id['aot.unit0'] || ''),
        'its one script unit was compiled: ' + JSON.stringify(id) + '\n' + readLog(exe));
    assert(id.id === 'org.bro.test.ExeApp' && id.name === 'Exe App' && id.version === '2.3.4',
        'the identity is stamped: ' + JSON.stringify(id));
    assert(norm(id.app) === norm(app), 'it names the app folder: ' + id.app);
    assert(id.state === 'current', 'fresh executable is current: ' + id.state);
    assert(id.engine && id.hash && id.libbro && id.bro, 'engine, hash, libbro and bro stamped: ' + JSON.stringify(id));
    if (isWin) {
        assert(id['win.FileDescription'] === 'Exe App', 'FileDescription is the app name: ' + id['win.FileDescription']);
        assert(id['win.ProductVersion'] === '2.3.4', 'ProductVersion: ' + id['win.ProductVersion']);
        assert(id['win.OriginalFilename'] === 'org-bro-test-ExeApp.exe',
            'OriginalFilename is the final name: ' + id['win.OriginalFilename']);
        assert(/\b16\b/.test(id['win.icon']) && /\b256\b/.test(id['win.icon']), 'icon sizes 16..256: ' + id['win.icon']);
    }
    const mtime = fs.statSync(exe).mtimeMs;
    const again = broCli(['--build-exe', app, '--out', exe]);
    assert(fs.statSync(exe).mtimeMs === mtime, 'a current executable is not rebuilt: ' + again);
    id = info(exe);
    assert(id['aot.current'] === '1' && id['aot.built'] === '0', 'nor is a current module: ' + JSON.stringify(id));

    // ---- a current executable runs the app as itself ------------------------------
    {
        const { report } = launch(exe);
        assert(report.id === 'org.bro.test.ExeApp', 'ran the app: ' + JSON.stringify(report));
        assert(norm(report.dir) === norm(app), 'from its folder: ' + report.dir);
        assert(report.executable && norm(report.executable) === norm(exe),
            'bro.app.executable is the app executable: ' + JSON.stringify(report));
        assert(norm(report.exePath) === norm(exe), 'and so is the process image: ' + report.exePath);
        assert(JSON.stringify(report.argv) === '["arg one"]', 'arguments pass through: ' + JSON.stringify(report.argv));
        assert(report.codeCache === 'aot', 'the page ran its ahead-of-time module: ' + report.codeCache);
        assert(/where/.test(report.stack) && /index\.html/.test(report.stack),
            'a stack trace from compiled code names its function and file: ' + report.stack);
    }

    // ---- ahead-of-time scripts: checked at each launch -----------------------------
    {
        const page = path.join(app, 'index.html');
        // Edited: the module is no longer that program, so the launch compiles
        // the page in process and rebuilds the directory behind it.
        fs.writeFileSync(page, fs.readFileSync(page, 'utf8').replace("marker: 'first'", "marker: 'second'"));
        assert(info(exe).state === 'current', 'a page edit leaves the executable current');
        let { report, stderr } = launch(exe);
        assert(report.marker === 'second', 'the edited page ran: ' + JSON.stringify(report));
        assert(report.codeCache !== 'aot', 'not from the stale module: ' + report.codeCache);
        assert(stderr.indexOf('out of date') >= 0, 'it says why: ' + stderr.slice(-1000));
        until(() => { try { return info(exe)['aot.built'] === '1'; } catch (e) { return false; } }, 60000,
            'the background rebuild of the AOT directory');
        report = launch(exe).report;
        assert(report.marker === 'second' && report.codeCache === 'aot',
            'the next launch runs the rebuilt module: ' + JSON.stringify(report));

        // A record this engine cannot read (another format or ABI): the same.
        broCli(['--build-exe', app, '--out', exe]);
        assert(info(exe)['aot.current'] === '1', 'current again');
        const records = fs.readdirSync(aotDirOf(exe)).filter((f) => f.endsWith('.bzm'));
        assert(records.length === 1, 'one record: ' + records);
        fs.writeFileSync(path.join(aotDirOf(exe), records[0]), 'not a record from this engine');
        ({ report, stderr } = launch(exe));
        assert(report.marker === 'second' && report.codeCache !== 'aot',
            'an unreadable record falls back to the JIT: ' + JSON.stringify(report));
        assert(stderr.indexOf('out of date') >= 0, 'and says so: ' + stderr.slice(-1000));
        until(() => { try { return info(exe)['aot.built'] === '1'; } catch (e) { return false; } }, 60000,
            'the rebuild after an unreadable record');
        assert(launch(exe).report.codeCache === 'aot', 'and the rebuilt module runs');

        // BRO_APP_AOT=0 runs without it (for comparing the two).
        const off = cp.spawnSync(exe, ['--no-splash'], {
            env: { ...baseEnv, BRO_APP_AOT: '0', BRO_TEST_EXE_OUT: path.join(scratch, 'off.json') },
            cwd: scratch, timeout: 90000, encoding: 'utf8' });
        assert(off.status === 0, 'BRO_APP_AOT=0 launch exited ' + off.status);
        const offReport = JSON.parse(fs.readFileSync(path.join(scratch, 'off.json'), 'utf8'));
        assert(offReport.codeCache !== 'aot', 'BRO_APP_AOT=0 compiles the page: ' + offReport.codeCache);
    }

    // ---- the app changed: run under bro, rebuild behind --------------------------
    {
        const manifest = JSON.parse(fs.readFileSync(path.join(app, 'bro.json'), 'utf8'));
        manifest.version = '2.3.5';
        fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify(manifest, null, 4));
        assert(info(exe).state !== 'current', 'a bro.json edit makes it stale');
        const { report, stderr } = launch(exe);
        assert(report.id === 'org.bro.test.ExeApp' && norm(report.dir) === norm(app),
            'a stale executable still runs the app: ' + JSON.stringify(report));
        assert(!report.executable, 'under the stock bro, not as itself: ' + JSON.stringify(report));
        assert(norm(report.exePath) === norm(windowed), 'the process is the stock bro: ' + report.exePath);
        assert(stderr.indexOf('out of date') >= 0, 'it says why: ' + stderr.slice(-1000));
        until(() => { try { return info(exe).state === 'current'; } catch (e) { return false; } }, 60000,
            'the background rebuild to finish');
        id = info(exe);
        assert(id.version === '2.3.5', 'rebuilt from the new bro.json: ' + id.version);
        const next = launch(exe).report;
        assert(next.executable && norm(next.executable) === norm(exe), 'the next launch runs the fresh one as itself');
    }

    // ---- built against another engine: falls back, never fails --------------------
    {
        broCli(['--build-exe', app, '--out', exe, '--force', '--stamp-engine', '0.0.1+abi1.1.1']);
        id = info(exe);
        assert(id.engine === '0.0.1+abi1.1.1' && id.state === 'built against another engine',
            'stamped with an old engine: ' + JSON.stringify(id));
        const { report } = launch(exe);
        assert(report.id === 'org.bro.test.ExeApp' && !report.executable,
            'an old executable runs the folder under the stock bro: ' + JSON.stringify(report));
        until(() => { try { return info(exe).state === 'current'; } catch (e) { return false; } }, 60000,
            'the rebuild against this engine');
    }

    // ---- --install builds it and points the launcher at it ------------------------
    {
        const data = path.join(scratch, 'install');
        const env = isWin ? { LOCALAPPDATA: data, APPDATA: path.join(scratch, 'roaming') } : { XDG_DATA_HOME: data };
        broCli(['--install', app], env);
        const installed = path.join(data, 'bro', 'apps', 'org.bro.test.ExeApp');
        const built = path.join(data, 'bro', 'bin', isWin ? 'org-bro-test-ExeApp.exe' : 'org-bro-test-ExeApp');
        assert(fs.existsSync(built), '--install built ' + built);
        const bi = info(built, env);
        assert(norm(bi.app) === norm(installed) && bi.state === 'current', 'for the installed copy: ' + JSON.stringify(bi));
        if (isWin) {
            const lnk = path.join(scratch, 'roaming', 'Microsoft', 'Windows', 'Start Menu', 'Programs', 'Bro Apps',
                'Exe App.lnk');
            assert(fs.existsSync(lnk), 'Start menu shortcut at ' + lnk);
            assert(bi['win.shortcut.AppUserModelID'] === 'org.bro.test.ExeApp',
                'the shortcut carries the AppUserModelID: ' + JSON.stringify(bi));
            assert(norm(bi['win.shortcut.target']) === norm(built), 'and starts the executable: ' + bi['win.shortcut.target']);
        } else {
            const de = fs.readFileSync(path.join(data, 'applications', 'org.bro.test.ExeApp.desktop'), 'utf8');
            const lines = de.split(/\r?\n/);
            const exec = lines.find((l) => l.startsWith('Exec='));
            assert(exec && (exec === 'Exec=' + built || exec.startsWith('Exec=' + built + ' ')),
                'Exec runs the executable: ' + exec);
            assert(lines.indexOf('StartupWMClass=org.bro.test.ExeApp') >= 0, 'StartupWMClass is the id');
        }
        broCli(['--uninstall', 'org.bro.test.ExeApp'], env);
        assert(!fs.existsSync(built), '--uninstall removed the executable');
        assert(!fs.existsSync(aotDirOf(built)), 'and its AOT directory');
        if (isWin) {
            assert(!fs.existsSync(path.join(scratch, 'roaming', 'Microsoft', 'Windows', 'Start Menu', 'Programs',
                'Bro Apps', 'Exe App.lnk')), 'and the shortcut');
        }
    }

    try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* a running rebuild may hold it */ }
}
