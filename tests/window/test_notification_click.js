// Notification clicks, action buttons and dismissals reach the page
// (docs/sys-api.js 5a). The desktop reports them (a toast's activator, D-Bus
// ActionInvoked / NotificationClosed, the notification center's delegate);
// headless stands in with clickNotification(id, action) and
// dismissNotification(id), delivered at the next frame as a real one is.
//   - a Notification the page holds and listens on gets `click` (with
//     event.action) or `close`;
//   - otherwise window gets `notificationclick` / `notificationclose` with a
//     notification rebuilt from what was posted, as a service worker would;
//   - a click brings the app's window forward (recorded headless: raised);
//   - a click that starts the app (`--notification <args>`) is heard as
//     window's notificationclick after load, its data intact.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const frame = () => advanceTime(16);
notifications({ clear: true });
notificationActivations({ clear: true });

assert(Notification.maxActions >= 2, 'actions are supported: maxActions ' + Notification.maxActions);

// --- A held Notification with buttons hears its clicks. ---
const n = new Notification('Download finished', {
    body: 'report.pdf',
    data: { path: '/tmp/report.pdf', size: 3 },
    actions: [{ action: 'open', title: 'Open' }, { action: 'folder', title: 'Show in folder' }],
});
assert(n.actions.length === 2 && n.actions[0].action === 'open' && n.actions[1].title === 'Show in folder',
    'actions read back: ' + JSON.stringify(n.actions));
frame();
const rec = notifications()[0];
assert(rec && rec.actions.length === 2 && rec.actions[0].action === 'open' && rec.actions[0].title === 'Open',
    'the desktop is given the buttons: ' + JSON.stringify(rec && rec.actions));
const posted = JSON.parse(rec.payload);
assert(posted.data.path === '/tmp/report.pdf' && posted.title === 'Download finished',
    'and what a click hands back: ' + rec.payload);

const clicks = [];
n.onclick = (e) => clicks.push({ action: e.action, same: e.notification === n, target: e.target === n, type: e.type });
assert(clickNotification(rec.id) === true, 'a posted notification can be clicked');
assert(clicks.length === 0, 'delivered at the next frame, not inside the call');
frame();
assert(clicks.length === 1 && clicks[0].action === '' && clicks[0].same && clicks[0].target && clicks[0].type === 'click',
    'a click on the notification itself: ' + JSON.stringify(clicks));
clickNotification(rec.id, 'folder');
frame();
assert(clicks.length === 2 && clicks[1].action === 'folder', 'a click on a button names it: ' + JSON.stringify(clicks));

let acts = notificationActivations();
assert(acts.length === 2 && acts.every((a) => a.type === 'click' && a.raised && !a.earlierRun && a.id === rec.id),
    'each click brought the window forward: ' + JSON.stringify(acts));

// The user closing it is `close`.
let closed = 0;
n.addEventListener('close', () => closed++);
assert(dismissNotification(rec.id) === true, 'dismiss');
frame();
assert(closed === 1, 'close fires when the user dismisses it: ' + closed);
acts = notificationActivations({ clear: true });
assert(acts[acts.length - 1].type === 'close' && !acts[acts.length - 1].raised, 'a dismissal raises nothing');

assert(clickNotification(987654) === false, 'an id never posted cannot be clicked');

// --- No listener on the Notification: window's notificationclick. ---
const winEvents = [];
const onWinClick = (e) => winEvents.push({ type: e.type, action: e.action, n: e.notification });
const onWinClose = (e) => winEvents.push({ type: e.type, n: e.notification });
window.addEventListener('notificationclick', onWinClick);
window.addEventListener('notificationclose', onWinClose);
const quiet = new Notification('Synced', { tag: 'sync', data: 42 });
frame();
const quietRec = notifications().find((r) => r.title === 'Synced');
clickNotification(quietRec.id);
frame();
assert(winEvents.length === 1 && winEvents[0].type === 'notificationclick' && winEvents[0].n === quiet &&
       winEvents[0].action === '' && winEvents[0].n.data === 42,
    'window hears a click the notification has no listener for: ' + JSON.stringify(winEvents.map((w) => w.type)));
dismissNotification(quietRec.id);
frame();
assert(winEvents.length === 2 && winEvents[1].type === 'notificationclose' && winEvents[1].n === quiet,
    'and its dismissal');

// --- A click that starts the app: bro --notification <args>. ---
const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');
const scratch = path.join(os.tmpdir(), 'bro_notification_click_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });
const app = path.join(scratch, 'app');
fs.mkdirSync(app);
fs.writeFileSync(path.join(app, 'bro.json'), JSON.stringify({ id: 'org.bro.test.NotifyClick', name: 'NotifyClick' }));
// The page listens at its top level, as an app would.
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><body><script>' +
    'window.heard = [];' +
    'window.addEventListener("notificationclick", (e) => window.heard.push({ action: e.action, title: e.notification.title, data: e.notification.data, actions: e.notification.actions.map((a) => a.action) }));' +
    '</script></body></html>');
const out = path.join(scratch, 'heard.json');
const script = path.join(scratch, 'check.js');
fs.writeFileSync(script,
    'advanceTime(16); advanceTime(16);\n' +
    'require("fs").writeFileSync(' + JSON.stringify(out) + ', JSON.stringify({ heard: window.heard, acts: notificationActivations() }));\n');
// What an earlier run posted, as the desktop hands it back (encodeNotificationArgs).
const payload = JSON.stringify({ title: 'Download finished', body: 'b', tag: '', data: { path: 'C:/x.pdf' },
    actions: [{ action: 'open', title: 'Open' }] });
const args = 'bro1;earlier-run;17;open;' + payload;
const env = { ...process.env, BRO_APP_HOME: path.join(scratch, 'home') };
const res = cp.spawnSync(headless, ['--notification', args, app, script], { env, encoding: 'utf8', timeout: 120000 });
assert(res.status === 0, 'the launched run passed: ' + res.status + '\n' + (res.stdout || '').slice(-2000) + (res.stderr || '').slice(-2000));
const got = JSON.parse(fs.readFileSync(out, 'utf8'));
assert(got.heard.length === 1 && got.heard[0].action === 'open' && got.heard[0].title === 'Download finished' &&
       got.heard[0].data.path === 'C:/x.pdf' && got.heard[0].actions[0] === 'open',
    'the launch click reached the page after load: ' + JSON.stringify(got));
assert(got.acts.length === 1 && got.acts[0].earlierRun === true && got.acts[0].id === 0 && got.acts[0].raised,
    'from an earlier run, and the window brought forward: ' + JSON.stringify(got.acts));
try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }

window.removeEventListener('notificationclick', onWinClick);
window.removeEventListener('notificationclose', onWinClose);
notifications({ clear: true });
notificationActivations({ clear: true });
console.log('test_notification_click.js PASSED');
