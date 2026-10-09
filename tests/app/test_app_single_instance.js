// The single-instance hand-off (docs/apps.md): with `"singleInstance": true`,
// a second launch hands its argv and working directory to the running
// instance, which hears an `instance` event on bro.app, and exits 0 without
// starting an engine. Two real processes: the primary (fixtures/si_primary.js)
// and the launch, both bro-headless --single-instance.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

// A private copy of the fixture with an id of its own, so a run never meets
// a channel another run (or the user) holds.
const scratch = path.join(os.tmpdir(), 'bro_si_test_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });
const app = path.join(scratch, 'si_app');
fs.mkdirSync(app);
const id = 'org.bro.test.SingleInstance' + process.pid + Date.now() % 100000;
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id, name: 'SI', singleInstance: true }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>si</body></html>');

const env = { ...process.env, BRO_APP_HOME: path.join(scratch, 'home') };
delete env.BRO_TRUSTED_APP_DIR;
const report = path.join(scratch, 'report.json');
env.BRO_TEST_SI_OUT = report;

// Simulated launches reach the same listeners (the in-process half).
{
    let got = null;
    bro.app.addEventListener('instance', (e) => { got = e; });
    bro.app._simulateInstance(['--x', 'y'], '/somewhere');
    advanceTime(16);
    assert(got && got.type === 'instance' && JSON.stringify(got.argv) === '["--x","y"]' && got.cwd === '/somewhere',
        'a simulated hand-off reaches addEventListener: ' + JSON.stringify(got));
}

const primary = cp.spawn(headless, ['--single-instance', app, path.resolve('tests/app/fixtures/si_primary.js')],
    { env, stdio: 'ignore' });
let primaryExit = null;
primary.on('exit', (code) => { primaryExit = code; });

const until = (pred, ms, what) => {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (pred()) return;
        advanceTime(16);
        wallSleep(25);
    }
    assert(pred(), 'timed out: ' + what);
};
until(() => fs.existsSync(report + '.ready'), 60000, 'the primary to come up');

// The second launch, from a different working directory.
const launchCwd = path.join(scratch, 'elsewhere');
fs.mkdirSync(launchCwd);
const t0 = Date.now();
const second = cp.spawnSync(headless, ['--single-instance', app, '--', '--new-tab', 'two words', ''],
    { env, cwd: launchCwd, encoding: 'utf8' });
const handoffMs = Date.now() - t0;
assert(second.status === 0, 'the second launch exits 0: status ' + second.status + ' ' + (second.stderr || ''));
assert(/handed off/.test(second.stderr || ''), 'it says it handed off: ' + second.stderr);

until(() => fs.existsSync(report), 30000, 'the primary to report the hand-off');
const got = JSON.parse(fs.readFileSync(report, 'utf8'));
assert(got.type === 'instance', 'an instance event');
assert(JSON.stringify(got.argv) === JSON.stringify(['--new-tab', 'two words', '']),
    'the argv arrived intact: ' + JSON.stringify(got.argv));
const norm = (p) => path.resolve(p).replace(/\\/g, '/').toLowerCase();
assert(norm(got.cwd) === norm(launchCwd), 'with the launch\'s working directory: ' + got.cwd);
console.log('test_app_single_instance: hand-off took ' + handoffMs + ' ms');

until(() => primaryExit !== null, 30000, 'the primary to exit');
assert(primaryExit === 0, 'the primary passed: exit ' + primaryExit);

try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
console.log('test_app_single_instance.js PASSED');
