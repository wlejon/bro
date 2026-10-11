// A notification click delivered to a bro that is not the app that posted it
// (docs/sys-api.js 5a). On macOS every bro app is one bundle, so whichever
// bro is running hears every app's clicks, and a click with none running
// starts a bare bro; the notification carries the posting app's directory
// and the click is routed (platform/desktop_notifications.h
// routeNotificationResponse, which notificationResponse() drives here):
//   - this app's own click is this app's (notificationclick);
//   - another app's click starts that app, `bro --notification <args> <dir>`
//     (headless records it: openedApps(), from 'notification'), carrying the
//     button and the payload;
//   - another app's dismissal concerns nobody running and is dropped;
//   - a running single-instance app is handed the click by that launch and
//     hears it as its own, with no `instance` event for it.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const frame = () => advanceTime(16);
notificationActivations({ clear: true });
openedApps({ clear: true });

const payload = JSON.stringify({ title: 'Build done', body: 'b', tag: '', data: { n: 7 },
    actions: [{ action: 'open', title: 'Open' }] });
const args = 'bro1;earlier-run;5;;' + payload;

// --- This app's own click. ---
const heard = [];
const onClick = (e) => heard.push({ action: e.action, title: e.notification.title, data: e.notification.data });
window.addEventListener('notificationclick', onClick);
assert(notificationResponse(process.env.BRO_APP_DIR, args, 'open') === 'queued', 'this app\'s click is its own');
frame();
assert(heard.length === 1 && heard[0].action === 'open' && heard[0].title === 'Build done' && heard[0].data.n === 7,
    'and its page hears it: ' + JSON.stringify(heard));
assert(notificationResponse('', args, '') === 'queued', 'a notification that names no app is this one\'s');
frame();
assert(heard.length === 2, 'heard too: ' + heard.length);
assert(openedApps().length === 0, 'nothing started for either');

// --- Another app's click starts that app. ---
const scratch = path.join(os.tmpdir(), 'bro_notification_route_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });
const other = path.join(scratch, 'other');
fs.mkdirSync(other);
assert(notificationResponse(other, args, 'open') === 'launched', 'another app\'s click starts it');
frame();
assert(heard.length === 2, 'this page hears nothing of it');
const opened = openedApps();
assert(opened.length === 1 && opened[0].from === 'notification' && opened[0].dir === other && !opened[0].spawned,
    'recorded, headless: ' + JSON.stringify(opened));
const cmd = opened[0].command;
assert(cmd.length === 4 && cmd[1] === '--notification' && cmd[3] === other,
    'as bro --notification <args> <dir>: ' + JSON.stringify(cmd));
assert(cmd[2].startsWith('bro1;') && cmd[2].split(';')[3] === 'open' && cmd[2].endsWith(payload),
    'the click\'s button and the payload ride along: ' + cmd[2]);

// --- Another app's dismissal, and text that is not a click. ---
assert(notificationResponse(other, args, '', true) === 'dropped', 'another app\'s dismissal is dropped');
assert(notificationResponse(other, 'not-a-click', '') === 'dropped', 'unreadable activation text is dropped');
assert(openedApps().length === 1, 'nothing more started');
window.removeEventListener('notificationclick', onClick);

// --- A running single-instance app is handed the click. ---
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');
const app = path.join(scratch, 'si_app');
fs.mkdirSync(app);
const id = 'org.bro.test.NotifyRoute' + process.pid + Date.now() % 100000;
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id, name: 'NR', singleInstance: true }));
fs.writeFileSync(path.join(app, 'index.html'), '<!DOCTYPE html><html><body>nr</body></html>');
const env = { ...process.env, BRO_APP_HOME: path.join(scratch, 'home') };
delete env.BRO_TRUSTED_APP_DIR;
const report = path.join(scratch, 'report.json');
env.BRO_TEST_SI_OUT = report;

const primary = cp.spawn(headless, ['--single-instance', app, path.resolve('tests/app/fixtures/si_notify_primary.js')],
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

const clickArgs = 'bro1;earlier-run;9;open;' + payload;
const second = cp.spawnSync(headless, ['--single-instance', '--notification', clickArgs, app], { env, encoding: 'utf8' });
assert(second.status === 0 && /handed off/.test(second.stderr || ''),
    'the click\'s launch hands off: ' + second.status + ' ' + (second.stderr || '').slice(-500));
until(() => fs.existsSync(report), 30000, 'the primary to report the click');
const got = JSON.parse(fs.readFileSync(report, 'utf8'));
assert(got.heard.length === 1 && got.heard[0].action === 'open' && got.heard[0].data.n === 7,
    'the running app heard the click: ' + JSON.stringify(got.heard));
assert(got.instances.length === 0, 'and no instance event for a launch that was only the click: ' +
    JSON.stringify(got.instances));
assert(got.acts.length === 1 && got.acts[0].earlierRun === true && got.acts[0].raised,
    'from an earlier run, the window brought forward: ' + JSON.stringify(got.acts));
until(() => primaryExit !== null, 30000, 'the primary to exit');
assert(primaryExit === 0, 'the primary passed: exit ' + primaryExit);

if (process.platform !== 'win32') {
    const runDir = process.env.XDG_RUNTIME_DIR || '/tmp';
    for (const f of fs.readdirSync(runDir).filter((n) => n.startsWith('bro_single_') && n.includes('_app-' + id + '.')))
        try { fs.unlinkSync(path.join(runDir, f)); } catch (e) { /* best effort */ }
}
try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
openedApps({ clear: true });
notificationActivations({ clear: true });
