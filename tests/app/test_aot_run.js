// `bro-headless <app> --aot <dir>` runs the page's scripts from the modules
// `--aot-build <dir>` wrote, as an installed app's executable runs them
// (docs/headless.md), so a test can cover the cycle installed apps go
// through (docs/apps.md, "Ahead-of-time scripts"): a unit has no module until
// its first run, compiled in process, has been harvested; the build after it
// is made on that profile, with the functions the run never entered cold;
// the next run runs that module; no module is ever built on nothing; a
// harvested compile is kept in the code cache under its own key, so another
// harvested launch of the same text is served from it; a run
// that enters a cold function often drops the profile, and the launch after
// it is harvested again; a module out of date fails the run instead of
// falling back to the JIT, and the run is harvested again; a directory that
// is not an AOT build is refused.

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_aot_run_'));
// The test's own code cache, so its first harvested compile is a miss.
const codeCache = path.join(root, 'code-cache');

function run(args) {
    const env = Object.assign({}, process.env, { BRO_APP_HOME: root, BRO_CODE_CACHE_DIR: codeCache });
    delete env.BRO_CODE_CACHE;
    const r = cp.spawnSync(headless, args, { encoding: 'utf-8', timeout: 120000, env });
    return { status: r.status, out: (r.stdout || '') + (r.stderr || '') };
}

function write(file, text) {
    fs.writeFileSync(file, text);
    return file;
}

const isModule = (f) => /\.(so|dll|dylib)$/.test(f);
const modules = (dir) => fs.readdirSync(dir).filter(isModule);
const profiles = (dir) => fs.readdirSync(dir).filter((f) => f.endsWith('.tfb'));
const units = (dir) => fs.readFileSync(path.join(dir, 'aot.txt'), 'utf-8').split('\n')
    .filter((l) => /^unit\d+=/.test(l));

// Every build a log reports is one on a profile.
function noPlainBuild(out, what) {
    const builds = out.split('\n').filter((l) => /aot: .*: built \d+ ms/.test(l));
    for (const l of builds) assert(l.includes('on its profile'), what + ' built a module on nothing: ' + l);
}

function main() {
    const app = path.join(root, 'app');
    const aot = path.join(root, 'aot');
    fs.mkdirSync(app);
    write(path.join(app, 'index.html'),
        '<!doctype html><html><body><script src="main.js"></script></body></html>\n');
    const source = 'function used(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return s; }\n' +
                   'function neverRun(o) { return o.a + o.b * 2; }\n' +
                   'globalThis.neverRun = neverRun;\n' +
                   'globalThis.total = 0;\n' +
                   'for (let k = 0; k < 2000; k++) globalThis.total += used(10);\n' +
                   'console.log("AOT_RUN page " + globalThis.total);\n';
    write(path.join(app, 'main.js'), source);
    const check = 'assert(globalThis.total === 90000, "the page ran: " + globalThis.total);\n' +
                  'console.log("AOT_RUN cache " + bro.app.startup.page.codeCache);\n';
    const probe = write(path.join(root, 'probe.js'), check);
    // Calls what the first run never did, as a later session might.
    const busyProbe = write(path.join(root, 'probe_busy.js'),
        check + 'let sum = 0;\nfor (let i = 0; i < 20000; i++) sum += globalThis.neverRun({ a: i, b: 1 });\n' +
        'assert(sum === 199990000 + 40000, "the cold function ran: " + sum);\n');

    // At install: nothing to build, and nothing is.
    const installed = run([app, '--aot-build', aot]);
    if (installed.status !== 0 && process.platform === 'darwin') {
        // App executables (and so AOT directories) are not built on macOS yet
        // (docs/apps.md); the module writer handles Mach-O, so this says why.
        skipTest('--aot-build failed on macOS: ' + installed.out.slice(-400));
        return;
    }
    assert(installed.status === 0, 'the AOT build succeeds: ' + installed.out.slice(-400));
    assert(units(aot).length === 1 && units(aot)[0].includes('waiting for its profile'),
        'the unit waits for its profile: ' + units(aot));
    assert(modules(aot).length === 0, 'no module is built without a profile: ' + modules(aot));

    // The first run: compiled in process, and its harvest is the unit's
    // profile.
    const first = run(['--aot', aot, app, probe]);
    assert(first.status === 0, 'the first run passes: ' + first.out.slice(-600));
    assert(first.out.includes('compiled in process, to be harvested'), 'the first run is harvested');
    assert(first.out.includes('AOT_RUN page 90000'), 'the first run ran the page');
    assert(first.out.includes('its run is harvested'), 'the harvest is the profile: ' + first.out.slice(-600));
    assert(profiles(aot).length === 1, 'the run left its profile: ' + fs.readdirSync(aot));
    assert(modules(aot).length === 0, 'a run builds nothing itself');

    // The build after it: on its profile, the functions the run never
    // entered cold.
    const built = run([app, '--aot-build', aot]);
    assert(built.status === 0, 'the build on its profile succeeds: ' + built.out.slice(-400));
    const cold = /built \d+ ms on its profile, (\d+) of (\d+) functions cold/.exec(units(aot)[0]);
    assert(cold, 'built on its profile: ' + units(aot)[0]);
    assert(Number(cold[1]) >= 1, 'a function the run never entered is cold: ' + units(aot)[0]);
    assert(modules(aot).length === 1, 'one module: ' + modules(aot));
    noPlainBuild(installed.out + built.out, 'the builds');

    // The second launch runs it.
    const second = run(['--aot', aot, app, probe]);
    assert(second.status === 0, 'the module built on its profile runs: ' + second.out.slice(-600));
    assert(second.out.includes('(built on its profile)'), 'the profiled module ran');
    assert(second.out.includes('AOT_RUN page 90000'), 'the module ran the page');
    assert(second.out.includes('AOT_RUN cache aot'), 'startup reports the page ran from its module');
    assert(!second.out.includes('to be harvested'), 'a module is not harvested');
    assert(!second.out.includes('harvested again'), 'a run like the profiled one keeps its profile');
    assert(profiles(aot).length === 1, 'the profile stays');

    // A harvested compile is kept in the code cache under a key of its own:
    // the first one missed it, and another harvested launch of the same text
    // (a fresh AOT directory, as after a dropped profile) is served from it,
    // still counts, and gives a profile the build takes as the first did.
    // The page's classic scripts are one unit, named by the page.
    const cacheLine = (out) => (/compiled \S*index\.html in \d+ ms \(code cache (\w+)/.exec(out) || [])[1];
    assert(cacheLine(first.out) === 'miss', 'the first harvested compile misses the cache: ' + cacheLine(first.out));
    const aotCached = path.join(root, 'aot_cached');
    assert(run([app, '--aot-build', aotCached]).status === 0, 'the second AOT directory installs');
    const cachedRun = run(['--aot', aotCached, app, probe]);
    assert(cachedRun.status === 0 && cachedRun.out.includes('compiled in process, to be harvested'),
        'the second directory\'s first run is harvested: ' + cachedRun.out.slice(-600));
    assert(cacheLine(cachedRun.out) === 'hit', 'the harvested compile is served from the cache: ' +
        cacheLine(cachedRun.out));
    assert(profiles(aotCached).length === 1, 'the cached run left its profile: ' + fs.readdirSync(aotCached));
    assert(run([app, '--aot-build', aotCached]).status === 0, 'the build on the cached run\'s profile succeeds');
    const coldCached = /built \d+ ms on its profile, (\d+) of (\d+) functions cold/.exec(units(aotCached)[0]);
    assert(coldCached && coldCached[1] === cold[1] && coldCached[2] === cold[2],
        'the cached run\'s profile builds as the first did: ' + units(aotCached)[0] + ' / ' + units(aot)[0]);
    const cachedModule = run(['--aot', aotCached, app, probe]);
    assert(cachedModule.status === 0 && cachedModule.out.includes('(built on its profile)'),
        'the module built on the cached run\'s profile runs: ' + cachedModule.out.slice(-600));

    // A run that enters a cold function often: the profile was too thin, and
    // the next launch is harvested again, then built on and run as before.
    const busy = run(['--aot', aot, app, busyProbe]);
    assert(busy.status === 0, 'the busy run passes: ' + busy.out.slice(-600));
    assert(busy.out.includes('(built on its profile)'), 'the busy run ran the module');
    assert(busy.out.includes('harvested again'), 'a cold function entered often drops the profile: ' +
        busy.out.slice(-600));
    assert(profiles(aot).length === 0, 'the thin profile is gone');
    const again = run(['--aot', aot, app, busyProbe]);
    assert(again.status === 0 && again.out.includes('compiled in process, to be harvested'),
        'the launch after it is harvested: ' + again.out.slice(-600));
    assert(run([app, '--aot-build', aot]).status === 0, 'the build on the new profile succeeds');
    assert(units(aot)[0].includes('on its profile'), 'built on the new profile: ' + units(aot)[0]);
    const after = run(['--aot', aot, app, busyProbe]);
    assert(after.status === 0 && after.out.includes('(built on its profile)'), 'the new module runs');
    assert(!after.out.includes('harvested again'), 'the new profile holds what the run does: ' +
        after.out.slice(-600));

    // Stale: an edited script is a failure of the run, said once, and the page
    // still runs, compiled and harvested again in place of the old profile.
    write(path.join(app, 'main.js'), source + '// edited\n');
    const stale = run(['--aot', aot, app, probe]);
    assert(stale.status !== 0, 'a run from an out-of-date module fails');
    const errors = stale.out.split('\n').filter((l) => l.includes('[ERROR]') && l.includes('is out of date'));
    assert(errors.length === 1, 'the stale module is reported once: ' + errors.length);
    assert(errors[0].includes('(compiled with other inputs (text))'), 'the refusal names what differs: ' + errors[0]);
    assert(stale.out.includes('AOT_RUN page 90000'), 'the page still ran, compiled');
    assert(stale.out.includes('compiled in process, to be harvested'), 'the edited unit is harvested again');
    assert(profiles(aot).length === 1, 'the edited run is the profile');
    const rebuilt = run([app, '--aot-build', aot]);
    assert(rebuilt.status === 0 && units(aot)[0].includes('on its profile'), 'the edited unit is built on it: ' +
        units(aot)[0]);
    const edited = run(['--aot', aot, app, probe]);
    assert(edited.status === 0 && edited.out.includes('(built on its profile)'), 'the edited unit runs its module');

    // Not an AOT build: refused before the engine starts.
    const none = run(['--aot', path.join(root, 'nothing'), app, probe]);
    assert(none.status !== 0 && none.out.includes('no aot.txt'), 'a directory without aot.txt is refused: ' + none.out);

    // Two module scripts: the first publishes its own file when it runs, so
    // the second is built against a registry that holds it and runs its
    // module, not one refused as out of date straight after its build.
    const app2 = path.join(root, 'app2');
    const aot2 = path.join(root, 'aot2');
    fs.mkdirSync(app2);
    write(path.join(app2, 'index.html'),
        '<!doctype html><html><body>' +
        '<script type="module" src="first.js"></script>' +
        '<script type="module" src="second.js"></script></body></html>\n');
    write(path.join(app2, 'first.js'), 'globalThis.first = 3;\n');
    write(path.join(app2, 'second.js'), 'globalThis.second = globalThis.first * 5;\n');
    const check2 = 'assert(globalThis.second === 15, "both modules ran: " + globalThis.second);\n';
    const probe2 = write(path.join(root, 'probe2.js'), check2);
    assert(run([app2, '--aot-build', aot2]).status === 0, 'the two-module AOT build succeeds');
    const first2 = run(['--aot', aot2, app2, probe2]);
    assert(first2.status === 0, 'the two-module first run passes: ' + first2.out.slice(-600));
    assert(profiles(aot2).length === 2, 'both units have their profile: ' + fs.readdirSync(aot2));
    const built2 = run([app2, '--aot-build', aot2]);
    assert(built2.status === 0, 'the two-module build succeeds: ' + built2.out.slice(-400));
    noPlainBuild(built2.out, 'the two-module build');
    const two = run(['--aot', aot2, app2, probe2]);
    assert(two.status === 0, 'a two-module page runs from its AOT build: ' + two.out.slice(-600));
    const ran = two.out.split('\n').filter((l) => l.includes('runs its ahead-of-time module'));
    assert(ran.length === 2, 'both module scripts ran their modules: ' + ran.length);
}
main();

try { fs.rmSync(root, { recursive: true, force: true }); } catch (e) {}
console.log('test_aot_run PASSED');
