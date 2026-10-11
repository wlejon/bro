// The running single-instance app in test_notification_route.js: holds the
// app's channel (bro-headless --single-instance), waits for a launch that was
// a click on one of its notifications, and writes what its page heard to
// $BRO_TEST_SI_OUT.
const fs = require('fs');
const out = process.env.BRO_TEST_SI_OUT;
assert(out, 'BRO_TEST_SI_OUT names the report file');
assert(bro.app.singleInstance === true, 'this process holds the instance channel');

const heard = [];
const instances = [];
window.addEventListener('notificationclick', (e) =>
    heard.push({ action: e.action, title: e.notification.title, data: e.notification.data }));
bro.app.addEventListener('instance', (e) => instances.push(e.argv));
fs.writeFileSync(out + '.ready', 'ready');

const end = Date.now() + 30000;
while (!heard.length && Date.now() < end) {
    advanceTime(16);
    wallSleep(10);
}
// A little longer, for an instance event that should not come.
for (let i = 0; i < 10; i++) { advanceTime(16); wallSleep(10); }
assert(heard.length, 'the handed-off click reached the page within 30 s');
fs.writeFileSync(out, JSON.stringify({ heard, instances, acts: notificationActivations() }));
