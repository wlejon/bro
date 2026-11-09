// `bro-headless <app> --aot <dir>` runs the page's scripts from the modules
// `--aot-build <dir>` wrote, as an installed app's executable runs them
// (docs/headless.md), so a test can cover the code installed apps run: a
// fresh module runs, a module out of date fails the run instead of falling
// back to the JIT, and a directory that is not an AOT build is refused.

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_aot_run_'));
const app = path.join(root, 'app');
const aot = path.join(root, 'aot');
fs.mkdirSync(app);
fs.writeFileSync(path.join(app, 'index.html'),
    '<!doctype html><html><body><script src="main.js"></script></body></html>\n');
const source = 'function square(n) { return n * n; }\n' +
               'globalThis.answer = square(7);\n' +
               'console.log("AOT_RUN page " + globalThis.answer);\n';
fs.writeFileSync(path.join(app, 'main.js'), source);
const probe = path.join(root, 'probe.js');
fs.writeFileSync(probe,
    'assert(globalThis.answer === 49, "the page ran: " + globalThis.answer);\n' +
    'console.log("AOT_RUN cache " + bro.app.startup.page.codeCache);\n');

function run(args) {
    const r = cp.spawnSync(headless, args, { encoding: 'utf-8', timeout: 120000,
                                            env: Object.assign({}, process.env, { BRO_APP_HOME: root }) });
    return { status: r.status, out: (r.stdout || '') + (r.stderr || '') };
}

function main() {
    const built = run([app, '--aot-build', aot]);
    if (built.status !== 0 && process.platform === 'darwin') {
        // App executables (and so AOT directories) are not built on macOS yet
        // (docs/apps.md); the module writer handles Mach-O, so this says why.
        skipTest('--aot-build failed on macOS: ' + built.out.slice(-400));
        return;
    }
    assert(built.status === 0, 'the AOT build succeeds: ' + built.out.slice(-400));
    assert(fs.existsSync(path.join(aot, 'aot.txt')), 'the build wrote aot.txt');

    // Fresh: the page unit runs its module.
    const fresh = run(['--aot', aot, app, probe]);
    assert(fresh.status === 0, 'a run from a fresh AOT build passes: ' + fresh.out.slice(-600));
    assert(fresh.out.includes('runs its ahead-of-time module'), 'the page ran its ahead-of-time module');
    assert(fresh.out.includes('AOT_RUN page 49'), 'the module ran the page');
    assert(fresh.out.includes('AOT_RUN cache aot'), 'startup reports the page ran from its module');

    // Stale: an edited script is a failure of the run, said once, and the page
    // still runs (compiled) so the rest of the output is there.
    fs.writeFileSync(path.join(app, 'main.js'), source + '// edited\n');
    const stale = run(['--aot', aot, app, probe]);
    assert(stale.status !== 0, 'a run from an out-of-date module fails');
    const errors = stale.out.split('\n').filter((l) => l.includes('[ERROR]') && l.includes('is out of date'));
    assert(errors.length === 1, 'the stale module is reported once: ' + errors.length);
    assert(stale.out.includes('AOT_RUN page 49'), 'the page still ran, compiled');

    // Not an AOT build: refused before the engine starts.
    const none = run(['--aot', path.join(root, 'nothing'), app, probe]);
    assert(none.status !== 0 && none.out.includes('no aot.txt'), 'a directory without aot.txt is refused: ' + none.out);
}
main();

try { fs.rmSync(root, { recursive: true, force: true }); } catch (e) {}
console.log('test_aot_run PASSED');
