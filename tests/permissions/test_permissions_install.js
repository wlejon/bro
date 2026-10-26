// Install identity (docs/apps.md, Permissions; src/engine/install_record.h):
// `bro --install` records a hash of what it installed, the grants the user
// gives the app attach to that hash, and an installed folder that is no
// longer what was installed asks again: changed in size or time (found at
// launch by a stat of each file), or changed with size and time put back
// (found after launch by the background hash, which takes the grants back
// while the app runs). A --link install records no content, so its grants
// hold across edits.
//
// Everything runs under one scratch BRO_APP_HOME: its install root is
// <home>/bro/apps and its grant store <home>/bro/grants.json.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const isWin = process.platform === 'win32';
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');
const windowed = path.join(path.dirname(headless), isWin ? 'bro.exe' : 'bro');

if (!fs.existsSync(windowed)) {
    skipTest('no windowed bro beside ' + headless + ' to install with');
} else {
    const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_perm_install_')));
    const home = path.join(scratch, 'home');
    const env = { ...process.env, BRO_APP_HOME: home };
    const id = 'org.bro.test.PermInstall';
    const src = path.join(scratch, 'src');
    fs.mkdirSync(path.join(src, 'js'), { recursive: true });
    fs.writeFileSync(path.join(src, 'bro.json'), JSON.stringify({ id, name: 'Install', permissions: ['net'] }));
    fs.writeFileSync(path.join(src, 'index.html'), '<!DOCTYPE html><html><body>install</body></html>');
    fs.writeFileSync(path.join(src, 'js', 'main.js'), '// version one\n');
    const installed = path.join(home, 'bro', 'apps', id);
    const record = installed + '.install.json';

    const bro = (args) => {
        const r = cp.spawnSync(windowed, args, { env, encoding: 'utf8', timeout: 120000 });
        return { status: r.status, out: (r.stdout || '') + (r.stderr || '') };
    };
    let scriptN = 0;
    const launch = (body) => {
        const script = path.join(scratch, 'probe' + (scriptN++) + '.js');
        fs.writeFileSync(script, 'const R = {};\n' + body + '\nconsole.log("RESULT:" + JSON.stringify(R));\n');
        const r = cp.spawnSync(headless, [installed, script], { env, encoding: 'utf8', timeout: 120000 });
        const out = (r.stdout || '') + (r.stderr || '');
        const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
        assert(line, 'the probe printed a result, got: ' + out.slice(-3000));
        const cost = /install record of \S+: (\w+)[^(]*\(([\d.]+) ms\)/.exec(out);
        return { R: JSON.parse(line.slice(line.indexOf('RESULT:') + 7)), out, check: cost && cost[1], ms: cost && +cost[2] };
    };
    const grantedNow = 'R.granted = bro.app.permissions.granted;\n';
    const store = () => JSON.parse(fs.readFileSync(path.join(home, 'bro', 'grants.json'), 'utf8'));

    // ---- installed and granted: the grant attaches to the record's hash -------------------
    let r = bro(['--install', src, '--no-exe']);
    assert(r.status === 0, 'bro --install: ' + r.out);
    assert(fs.existsSync(record), 'the install wrote its record beside the folder');
    const rec = JSON.parse(fs.readFileSync(record, 'utf8'));
    assert(/^[0-9a-f]{64}$/.test(rec.hash) && rec.files.length === 3, 'it hashes every file: ' + JSON.stringify(rec));
    r = bro(['--grant-app', id, 'net']);
    assert(r.status === 0, '--grant-app: ' + r.out);
    assert(store().apps['install:' + id].installHash === rec.hash, 'the decision is kept with the hash: ' + JSON.stringify(store()));
    let l = launch(grantedNow);
    assert(l.R.granted.indexOf('net') >= 0, 'the installed app has net: ' + JSON.stringify(l.R));
    assert(l.check === 'matches', 'the launch checked the record: ' + l.check);
    console.log('test_permissions_install: launch check of a 3-file install took ' + l.ms + ' ms');

    // ---- changed in place: the launch sees it and asks again ------------------------------
    fs.appendFileSync(path.join(installed, 'js', 'main.js'), '// tampered\n');
    l = launch(grantedNow);
    assert(l.R.granted.indexOf('net') < 0, 'altered, it is not granted net: ' + JSON.stringify(l.R));
    assert(/asks again/.test(l.out), 'and the log says it asks again');
    r = bro(['--permissions', id]);
    assert(/changed/.test(r.out), '--permissions shows the install changed: ' + r.out);

    // ---- installed again: a new install is new content, decided anew ---------------------
    r = bro(['--install', src, '--no-exe']);
    assert(r.status === 0, 'reinstall: ' + r.out);
    r = bro(['--grant-app', id, 'net']);
    l = launch(grantedNow);
    assert(l.R.granted.indexOf('net') >= 0, 'decided for the new install, it has net: ' + JSON.stringify(l.R));

    // ---- changed with size and time put back: the background hash finds it ----------------
    {
        const file = path.join(installed, 'js', 'main.js');
        const st = fs.statSync(file);
        const text = fs.readFileSync(file, 'utf8');
        fs.writeFileSync(file, text.replace('one', 'two'));   // same length
        fs.utimesSync(file, st.atime, st.mtime);
        l = launch(
            'R.at = bro.app.permissions.granted.indexOf("net") >= 0;\n' +
            'let waited = 0;\n' +
            'while (bro.app.permissions.granted.indexOf("net") >= 0 && waited < 10000) { wallSleep(50); advanceTime(16); waited += 50; }\n' +
            'R.waited = waited;\n' +
            'R.after = bro.app.permissions.granted.indexOf("net") >= 0;\n' +
            'R.fetch = "";\n' +
            'fetch("http://127.0.0.1:9/").then(() => { R.fetch = "ok"; }, (e) => { R.fetch = e.name + ": " + e.message; });\n' +
            'for (let i = 0; i < 10; i++) advanceTime(25);\n');
        if (l.check === 'matches') {
            assert(l.R.after === false && l.R.waited < 10000, 'the background check took net back while it ran: ' + JSON.stringify(l.R));
            assert(/^PermissionRevoked: /.test(l.R.fetch), 'its next fetch is a PermissionRevoked: ' + l.R.fetch);
        } else {
            // A file system whose time could not be put back exactly: the
            // launch's stat saw it instead.
            assert(l.R.at === false, 'altered, it was not granted at launch: ' + JSON.stringify(l.R));
        }
    }

    // ---- a --link install: grants hold across edits ----------------------------------------
    r = bro(['--uninstall', id]);
    assert(r.status === 0 && !fs.existsSync(record), 'uninstall removes the record too: ' + r.out);
    r = bro(['--install', src, '--link', '--no-exe']);
    if (r.status !== 0 && /cannot link/.test(r.out)) {
        console.log('test_permissions_install: no symlinks here (' + r.out.trim() + '); --link not tested');
    } else {
        assert(r.status === 0, 'bro --install --link: ' + r.out);
        assert(JSON.parse(fs.readFileSync(record, 'utf8')).linked === true, 'the record says linked');
        r = bro(['--grant-app', id, 'net']);
        fs.appendFileSync(path.join(src, 'js', 'main.js'), '// edited in the checkout\n');
        l = launch(grantedNow);
        assert(l.R.granted.indexOf('net') >= 0, 'an edited linked install keeps its grant: ' + JSON.stringify(l.R));
    }

    try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
}
