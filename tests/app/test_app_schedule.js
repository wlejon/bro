// bro.app.schedule(id, when, {args, wake}) / unschedule(id) / scheduled() /
// scheduledLaunch: launches the OS starts at a time, with the app closed
// (engine/app_schedule.h, platform/app_schedule.h).
//
// 1. This run is headless, which records schedules and reaches no OS (as
//    bro.app.open starts nothing): the shapes, ids, moving and removing.
// 2. A child run granted nothing is refused (the `schedule` capability).
// 3. A child run with BRO_SCHEDULE_OS=1 schedules a real launch of a scratch
//    app a few seconds out (Task Scheduler / a systemd user timer / a launchd
//    agent); the launch runs bro-headless on that app, whose page writes what
//    it was started with. The launch removes its own OS entry, and the test
//    removes it too whatever happened. launchd's calendar is in whole
//    minutes, so on macOS that wait is up to a minute.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

assert(typeof bro.app.schedule === 'function' && typeof bro.app.unschedule === 'function' &&
       typeof bro.app.scheduled === 'function', 'bro.app.schedule / unschedule / scheduled exist');
assert(bro.app.scheduledLaunch === null, 'this run was not started by a schedule');

let pending = 0;
let failure = null;
function settleAll(work) {
    pending++;
    work().then(() => { pending--; }, (e) => { failure = failure || e; pending--; });
}
function pump(maxFrames) {
    for (let i = 0; i < maxFrames && pending > 0; ++i) advanceTime(16);
    if (failure) throw failure;
    assert(pending === 0, 'the promises settled');
}
async function rejection(p) {
    try { await p; } catch (e) { return e; }
    return null;
}

// ---- 1. recorded --------------------------------------------------------------
settleAll(async () => {
    for (const s of bro.app.scheduled()) bro.app.unschedule(s.id);
    const when = Date.now() + 60000;
    const r = await bro.app.schedule('wake-up', new Date(when), { args: ['--alarm', 'wake-up'] });
    assert(r.id === 'wake-up' && Math.abs(r.when - when) < 1000, 'the launch: ' + JSON.stringify(r));
    assert(r.backend === 'recorded', 'headless records: ' + r.backend);
    assert(Array.isArray(r.args) && r.args.join(' ') === '--alarm wake-up', 'its args');
    const i = r.command.indexOf('--scheduled');
    assert(i === 1 && r.command[r.command.length - 1] === 'wake-up', 'the command marks the launch: ' + r.command.join(' '));
    assert(r.wake === false, 'wake defaults off');

    // A number and a string are times too; the same id moves the launch.
    await bro.app.schedule('later', when + 120000);
    await bro.app.schedule('wake-up', new Date(when + 5000).toISOString(), { wake: true });
    const list = bro.app.scheduled();
    assert(list.length === 2 && list[0].id === 'wake-up' && list[1].id === 'later', 'soonest first: ' + JSON.stringify(list.map((s) => s.id)));
    assert(Math.abs(list[0].when - (when + 5000)) < 1000 && list[0].wake === true && list[0].args.length === 0,
           'rescheduling replaces: ' + JSON.stringify(list[0]));

    assert(bro.app.unschedule('wake-up') === true, 'unschedule: there was one');
    assert(bro.app.unschedule('wake-up') === false, 'unschedule: no longer');
    assert(bro.app.unschedule('later') === true && bro.app.scheduled().length === 0, 'all gone');

    let e = await rejection(bro.app.schedule('bad id!', when));
    assert(e && e.code === 'EINVAL', 'a bad id is EINVAL: ' + (e && e.message));
    e = await rejection(bro.app.schedule('past', Date.now() - 60000));
    assert(e && e.code === 'EINVAL', 'the past is EINVAL: ' + (e && e.message));
    e = await rejection(bro.app.schedule('nowhen', 'not a date'));
    assert(e && e.code === 'EINVAL', 'an unreadable time is EINVAL: ' + (e && e.message));
});
pump(50);

// ---- child runs -------------------------------------------------------------------
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless to run children');
const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'bro_schedule_')));

function child(appDir, args, script, env) {
    const file = path.join(scratch, 'script' + Math.random().toString(36).slice(2) + '.js');
    fs.writeFileSync(file,
        'let OUT = null;\n' +
        '(async () => {\n' + script + '\n})().then((v) => { OUT = { ok: v === undefined ? null : v }; }, ' +
        '(e) => { OUT = { code: e.code, message: e.message }; });\n' +
        'for (let i = 0; i < 100 && OUT === null; ++i) advanceTime(16);\n' +
        'console.log("RESULT:" + JSON.stringify(OUT));\n');
    const r = cp.spawnSync(headless, args.concat([appDir, file]),
                           { encoding: 'utf8', timeout: 120000, env: { ...process.env, ...(env || {}) } });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the child reported: ' + out.slice(-800));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}

// The scratch app: its page writes what a launch gave it to the file named
// by its first argument.
const appId = 'org.bro.test.Scheduled' + (process.pid % 100000);
const app = path.join(scratch, 'app');
fs.mkdirSync(app, { recursive: true });
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: appId, name: 'Scheduled', permissions: ['schedule'] }));
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script>\n' +
    'if (bro.app.scheduledLaunch && bro.app.argv.length) {\n' +
    '  require("fs").writeFileSync(bro.app.argv[0], JSON.stringify({ launch: bro.app.scheduledLaunch, argv: bro.app.argv }));\n' +
    '}\n' +
    '</script></body></html>');

// ---- 2. the capability ------------------------------------------------------------
{
    const r = child(app, ['--no-splash'], 'return await bro.app.schedule("x", Date.now() + 60000);');
    assert(r.code === 'EACCES', 'undecided and not granted: refused: ' + JSON.stringify(r));
}

// ---- 3. a real launch ---------------------------------------------------------------
const marker = path.join(scratch, 'launched.json');
const realEnv = { BRO_SCHEDULE_OS: '1' };
const cleanup = () => child(app, ['--no-splash', '--grant', 'schedule'],
                            'return bro.app.unschedule("ring");', realEnv);
try {
    const s = child(app, ['--no-splash', '--grant', 'schedule'],
                    'const r = await bro.app.schedule("ring", Date.now() + 3000, { args: [' + JSON.stringify(marker) + ', "--ring"] });\n' +
                    'return { r, list: bro.app.scheduled() };', realEnv);
    if (s.code === 'EIO' && /cannot|not available|session bus|GUI domain/.test(s.message || '')) {
        console.log('test_app_schedule: SKIP the real launch (' + s.message + ')');
    } else {
        assert(s.ok && s.ok.r && s.ok.r.id === 'ring', 'scheduled for real: ' + JSON.stringify(s));
        const backend = s.ok.r.backend;
        assert(['task-scheduler', 'systemd', 'launchd'].includes(backend), 'an OS backend: ' + backend);
        assert(s.ok.list.length === 1 && s.ok.list[0].id === 'ring', 'scheduled() lists it: ' + JSON.stringify(s.ok.list));
        const limitMs = backend === 'launchd' ? 75000 : 30000;
        const start = Date.now();
        while (!fs.existsSync(marker) && Date.now() - start < limitMs) sleep(250);
        assert(fs.existsSync(marker), 'the ' + backend + ' launch ran within ' + limitMs / 1000 + ' s');
        sleep(500);  // the page's write
        const got = JSON.parse(fs.readFileSync(marker, 'utf8'));
        assert(got.launch && got.launch.id === 'ring', 'bro.app.scheduledLaunch names it: ' + JSON.stringify(got));
        assert(got.argv.join(' ') === marker + ' --ring', 'the launch has its args: ' + JSON.stringify(got.argv));
        console.log('test_app_schedule: ' + backend + ' launched the app ' + ((Date.now() - start) / 1000).toFixed(1) + ' s after waiting began');
        const after = child(app, ['--no-splash', '--grant', 'schedule'], 'return bro.app.scheduled();', realEnv);
        assert(after.ok && after.ok.length === 0, 'the launch removed itself: ' + JSON.stringify(after));
    }
} finally {
    cleanup();
    fs.rmSync(scratch, { recursive: true, force: true });
}
console.log('test_app_schedule OK');
