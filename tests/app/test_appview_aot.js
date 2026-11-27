// An <appview src=child> compartment loads, harvests, builds, and reuses AOT
// modules under --aot <dir> just like top-level apps and Workers.
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_appview_aot_'));
const codeCache = path.join(root, 'code-cache');

function run(args) {
    const env = Object.assign({}, process.env, { BRO_APP_HOME: root, BRO_CODE_CACHE_DIR: codeCache });
    delete env.BRO_CODE_CACHE;
    const r = cp.spawnSync(headless, args, { encoding: 'utf-8', timeout: 120000, env });
    const out = (r.stdout || '') + (r.stderr || '');
    assert(!/bro-headless crash/.test(out), 'no crash in ' + args.join(' ') + ': ' + out.slice(-1500));
    return { status: r.status, out };
}

function write(file, text) {
    fs.writeFileSync(file, text);
    return file;
}

function main() {
    const hostApp = path.join(root, 'host');
    const childApp = path.join(root, 'child');
    const parentAot = path.join(root, 'host-aot');
    fs.mkdirSync(hostApp);
    fs.mkdirSync(childApp);

    write(path.join(childApp, 'bro.json'), JSON.stringify({ id: 'org.bro.test.AppViewAotChild', name: 'AppView AOT Child' }));
    write(path.join(childApp, 'index.html'), '<!doctype html><html><body><script type="module" src="child.js"></script></body></html>\n');
    write(path.join(childApp, 'child.js'),
        'function sumHot(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return s; }\n' +
        'function neverCalled(x) { return x.a + x.b; }\n' +
        'let acc = 0; for (let k = 0; k < 2000; k++) acc += sumHot(20);\n' +
        'parent.postMessage({ ready: true, acc }, "*");\n');

    write(path.join(hostApp, 'index.html'),
        '<!doctype html><html><body>' +
        '<appview id="av" src="../child" style="display:block;width:200px;height:100px"></appview>' +
        '<script type="module" src="host.js"></script></body></html>\n');
    write(path.join(hostApp, 'host.js'),
        'window.__avMsg = null;\nwindow.addEventListener("message", (e) => { window.__avMsg = e.data; });\n');

    const probe = write(path.join(root, 'probe.js'),
        'for (let i = 0; i < 60 && !window.__avMsg; i++) runFrames(1);\n' +
        'assert(window.__avMsg && window.__avMsg.ready && window.__avMsg.acc === 380000,\n' +
        '       "appview computed expected result: " + JSON.stringify(window.__avMsg));\n');

    const initBuild = run([hostApp, '--aot-build', parentAot]);
    if (initBuild.status !== 0 && process.platform === 'darwin') {
        skipTest('--aot-build failed on macOS');
        return;
    }
    assert(initBuild.status === 0, 'host initial --aot-build succeeds: ' + initBuild.out.slice(-400));

    // 1. First run harvests both host and <appview> scripts.
    const first = run(['--aot', parentAot, hostApp, probe]);
    assert(first.status === 0, 'first --aot run passes: ' + first.out.slice(-1000));
    const appviewsDir = path.join(parentAot, 'appviews');
    assert(fs.existsSync(appviewsDir), 'appviews AOT dir created under parent AOT dir');
    const subdirs = fs.readdirSync(appviewsDir);
    assert(subdirs.length === 1, 'one appview AOT subdir: ' + subdirs);
    const childAotDir = path.join(appviewsDir, subdirs[0]);
    const tfbs = fs.readdirSync(childAotDir).filter((f) => f.endsWith('.tfb'));
    assert(tfbs.length >= 1, 'child appview harvested .tfb profile: ' + fs.readdirSync(childAotDir));

    // 2. Build both host and child AOT dirs.
    const hostBuilt = run([hostApp, '--aot-build', parentAot]);
    assert(hostBuilt.status === 0, 'host rebuild succeeds: ' + hostBuilt.out.slice(-400));
    const childBuilt = run([childApp, '--aot-build', childAotDir]);
    assert(childBuilt.status === 0, 'child --aot-build succeeds: ' + childBuilt.out.slice(-400));
    const mods = fs.readdirSync(childAotDir).filter((f) => /\.(so|dll|dylib)$/.test(f));
    assert(mods.length >= 1, 'child AOT module emitted: ' + fs.readdirSync(childAotDir));

    // 3. Second run loads both host and child AOT modules.
    const second = run(['--aot', parentAot, hostApp, probe]);
    assert(second.status === 0, 'second --aot run passes: ' + second.out.slice(-1000));
    assert(/child\.js runs its ahead-of-time module/.test(second.out),
           'child appview loaded its AOT module:\n' + second.out.slice(-1200));
    assert(!/out of date/.test(second.out), 'no AOT record mismatch:\n' + second.out.slice(-1200));
}

try {
    main();
    console.log('test_appview_aot OK');
} finally {
    fs.rmSync(root, { recursive: true, force: true });
}
