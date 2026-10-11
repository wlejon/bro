// The web Notification API (docs/sys-api.js): an app posts a desktop
// notification as a page does, and bro shows it under the app's own id and
// name (a toast on Windows, the notification center on macOS, D-Bus on
// Linux). Headless records it instead of showing it: notifications().

assert(typeof Notification === 'function', 'Notification exists');
assert(Notification.permission === 'granted', 'a desktop app may notify: ' + Notification.permission);
assert(typeof notifications === 'function', 'headless notifications()');
notifications({ clear: true });

let perm = null;
Notification.requestPermission().then((p) => { perm = p; });
advanceTime(16);
assert(perm === 'granted', 'requestPermission resolves granted: ' + perm);

let threw = false;
try { Notification('x'); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'Notification without new throws a TypeError');

const events = [];
const n = new Notification('Download finished', { body: 'report.pdf', tag: 'dl', silent: true });
n.onshow = () => events.push('show');
n.addEventListener('close', () => events.push('close'));
assert(n.title === 'Download finished' && n.body === 'report.pdf' && n.tag === 'dl' && n.silent === true,
    'the options read back');
advanceTime(16);
assert(events.join() === 'show', 'show fires once the desktop took it: ' + events.join());

let list = notifications();
assert(list.length === 1, 'one notification recorded: ' + JSON.stringify(list));
const rec = list[0];
assert(rec.title === 'Download finished' && rec.body === 'report.pdf', 'title and body');
assert(rec.silent === true, 'silent');
assert(rec.via === 'headless', 'headless shows nothing, only records: ' + rec.via);
assert(rec.appId === (bro.app.id || '') && rec.appName === (bro.app.name || ''),
    'under the app\'s identity: ' + rec.appId + ' / ' + rec.appName + ' vs ' + bro.app.id + ' / ' + bro.app.name);

// The same tag replaces the earlier one.
const n2 = new Notification('Download finished', { body: 'report2.pdf', tag: 'dl', requireInteraction: true });
advanceTime(16);
list = notifications();
assert(list.length === 2 && list[1].replacesId === list[0].id, 'a repeated tag replaces: ' + JSON.stringify(list));
assert(list[1].timeout === 0, 'requireInteraction stays up (timeout 0): ' + list[1].timeout);

n.close();
advanceTime(16);
assert(events.join() === 'show,close', 'close fires: ' + events.join());
n2.close();

notifications({ clear: true });
assert(notifications().length === 0, 'cleared');
