// The primary instance in test_app_single_instance.js: holds the app's
// channel (bro-headless --single-instance), waits for one handed-off launch,
// and writes what it received to $BRO_TEST_SI_OUT.
const fs = require('fs');
const out = process.env.BRO_TEST_SI_OUT;
assert(out, 'BRO_TEST_SI_OUT names the report file');
assert(bro.app.singleInstance === true, 'this process holds the instance channel');

let got = null;
bro.app.addEventListener('instance', (e) => { got = e; });
// Ready: the test starts the second launch once this file exists.
fs.writeFileSync(out + '.ready', 'ready');

const end = Date.now() + 30000;
while (!got && Date.now() < end) {
    advanceTime(16);
    wallSleep(10);
}
assert(got, 'a second launch handed off within 30 s');
fs.writeFileSync(out, JSON.stringify({ type: got.type, argv: got.argv, cwd: got.cwd }));
