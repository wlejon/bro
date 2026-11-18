// localStorage isolates per BRO_APP_HOME: two clients running the same app
// with distinct BRO_APP_HOME profiles never read/overwrite the app's real
// <appDir>/.storage.json and never collide with each other. Child iframe
// documents preserve their relative subpath under <BRO_APP_HOME>/data/.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the child');

const scratch = path.join(os.tmpdir(), 'bro_storage_home_' + process.pid + '_' + Date.now());
const appDir = path.join(scratch, 'app');
const subDir = path.join(appDir, 'sub');
const homeA = path.join(scratch, 'homeA');
const homeB = path.join(scratch, 'homeB');

fs.mkdirSync(subDir, { recursive: true });
fs.mkdirSync(homeA, { recursive: true });
fs.mkdirSync(homeB, { recursive: true });

fs.writeFileSync(path.join(appDir, 'index.html'), '<!DOCTYPE html><html><body><div id="root"></div></body></html>');
fs.writeFileSync(
    path.join(subDir, 'index.html'),
    '<!DOCTYPE html><html><body><script>' +
    'var n = parseInt(localStorage.getItem("subRuns") || "0", 10) + 1;' +
    'window.__seenRealChild = localStorage.getItem("realChild");' +
    'localStorage.setItem("subRuns", String(n));' +
    '</script></body></html>'
);

const sentinelMain = '{"realSave":"keep_me"}';
const sentinelSub = '{"realChild":"keep_child"}';
fs.writeFileSync(path.join(appDir, '.storage.json'), sentinelMain);
fs.writeFileSync(path.join(subDir, '.storage.json'), sentinelSub);

function runChild(home, js) {
    const env = { ...process.env, BRO_APP_HOME: home };
    const out = cp.execFileSync(headless, [appDir, '-e', js], { env, encoding: 'utf8' });
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'child produced RESULT line, got:\n' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}

const childScript = (val) =>
    'const beforeReal = localStorage.getItem("realSave");' +
    'const beforeClient = localStorage.getItem("client");' +
    'const lenBefore = localStorage.length;' +
    'localStorage.setItem("client", ' + JSON.stringify(val) + ');' +
    'const el = document.createElement("iframe");' +
    'el.setAttribute("src", "sub");' +
    'document.body.appendChild(el);' +
    'flush();' +
    'console.log("RESULT:" + JSON.stringify({ beforeReal, beforeClient, lenBefore }));';

// Client A: clean slate, does not see realSave, persists to homeA/data/
const rA1 = runChild(homeA, childScript('A'));
assert(rA1.beforeReal === null, 'homeA must not load realSave, got ' + rA1.beforeReal);
assert(rA1.beforeClient === null, 'homeA starts empty');
assert(rA1.lenBefore === 0, 'homeA localStorage.length starts 0, got ' + rA1.lenBefore);

const norm = (p) => path.resolve(p).replace(/\\/g, '/');
assert(fs.existsSync(path.join(homeA, 'data', '.storage.json')),
       'homeA/data/.storage.json created');
assert(fs.existsSync(path.join(homeA, 'data', 'sub', '.storage.json')),
       'homeA/data/sub/.storage.json created');
assert(JSON.parse(fs.readFileSync(path.join(homeA, 'data', '.storage.json'), 'utf8')).client === 'A',
       'homeA top-level storage saved client=A');
assert(JSON.parse(fs.readFileSync(path.join(homeA, 'data', 'sub', '.storage.json'), 'utf8')).subRuns === '1',
       'homeA child storage saved subRuns=1');

// Client B: isolated from both realSave and Client A
const rB1 = runChild(homeB, childScript('B'));
assert(rB1.beforeReal === null, 'homeB must not load realSave');
assert(rB1.beforeClient === null, 'homeB must not see homeA client, got ' + rB1.beforeClient);
assert(JSON.parse(fs.readFileSync(path.join(homeB, 'data', '.storage.json'), 'utf8')).client === 'B',
       'homeB top-level storage saved client=B');

// Client A relaunch: sees its own client='A' and child subRuns increments to 2
const rA2 = runChild(homeA, childScript('A2'));
assert(rA2.beforeClient === 'A', 'homeA persisted client=A across relaunch, got ' + rA2.beforeClient);
assert(JSON.parse(fs.readFileSync(path.join(homeA, 'data', 'sub', '.storage.json'), 'utf8')).subRuns === '2',
       'homeA child storage persisted across relaunch (subRuns=2)');
assert(JSON.parse(fs.readFileSync(path.join(homeB, 'data', 'sub', '.storage.json'), 'utf8')).subRuns === '1',
       'homeB child storage remained untouched (subRuns=1)');

// App directory's real .storage.json files were never read or overwritten
assert(fs.readFileSync(path.join(appDir, '.storage.json'), 'utf8') === sentinelMain,
       appDir + ' .storage.json was NOT clobbered');
assert(fs.readFileSync(path.join(subDir, '.storage.json'), 'utf8') === sentinelSub,
       subDir + ' .storage.json was NOT clobbered');

fs.rmSync(scratch, { recursive: true, force: true });
console.log('test_storage_app_home.js PASSED');
