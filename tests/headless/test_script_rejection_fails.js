// A headless script run FAILS when a promise rejection escapes it: a
// top-level await that rejects (before or after the top level returned), a
// rejection nothing handled (an `unhandledrejection` nobody cancelled), or a
// top-level await that never settles, so the rest of the script never ran.
// Each case is a fixture run in a child bro-headless; its exit status is the
// verdict tests/run_tests.sh reads.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = process.env.BRO_APP_DIR;
const fixtures = path.join(appDir, '..', 'headless', 'script_fixtures');

function run(name, extraEnv) {
    const env = Object.assign({}, process.env, extraEnv || {});
    const r = cp.spawnSync(exe, [appDir, path.join(fixtures, name)],
                           { encoding: 'utf8', env });
    return { status: r.status, out: (r.stdout || '') + (r.stderr || '') };
}

function expectFail(name, needle, extraEnv) {
    const r = run(name, extraEnv);
    assert(r.status !== 0, name + ' must fail the run, exited ' + r.status +
           '\n--- child output ---\n' + r.out);
    if (needle) {
        assert(r.out.includes(needle), name + ': expected "' + needle +
               '" in the output\n--- child output ---\n' + r.out);
    }
    assert(!r.out.includes('FIXTURE_UNREACHABLE'), name + ' ran past its failure');
    return r;
}

expectFail('tla_rejects.js', 'fixture: tla rejected 1');
const late = expectFail('tla_rejects_after_timer.js', 'fixture: failed after the timer');
assert(late.out.includes('FIXTURE_RESUMED'),
       'the script resumed after its timer instead of being abandoned');
expectFail('unhandled_rejection.js', 'fixture: nobody handled this');
expectFail('module_tla_rejects.js', 'fixture: module tla rejected');
expectFail('tla_never_settles.js', 'never settled', { BRO_SCRIPT_SETTLE_MS: '400' });

const ok = run('settles_cleanly.js');
assert(ok.status === 0, 'a script that settles and cancels its report passes, exited ' +
       ok.status + '\n--- child output ---\n' + ok.out);
assert(ok.out.includes('FIXTURE_RESUMED 42'), 'it ran to its end:\n' + ok.out);

console.log('script rejection verdicts OK');
