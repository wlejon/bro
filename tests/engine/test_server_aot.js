// bro-server --aot <dir>: the server script is the app's one unit, built ahead
// of time on its own first run as an installed app's page is (docs/apps.md,
// "Ahead-of-time scripts"). The first run compiles in process and is
// harvested, and at its end starts `bro-server <app> <script> --aot-build
// <dir>` in the background; the next run runs the module. The app's page is
// never run, harvested or built.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-server.exe' : 'bro-server';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_server_aot_'));
const app = path.join(root, 'app');
const aot = path.join(root, 'aot');
fs.mkdirSync(app);

fs.writeFileSync(path.join(app, 'index.html'),
    '<!doctype html><html><body><script>console.log("SERVER_AOT_PAGE_RAN")</script></body></html>');
fs.writeFileSync(path.join(app, 'rules.js'),
    'export function score(n) { let s = 0; for (let i = 0; i < n; i++) s += i % 7; return s; }\n');
fs.writeFileSync(path.join(app, 'server.js'),
    'import { score } from "./rules.js";\n' +
    'let total = 0;\nfor (let k = 0; k < 400; k++) total += score(1000);\n' +
    'console.log("SERVER_AOT total=" + total);\n' +
    'setTimeout(() => bro.server.stop(), 50);\n');

function run(args) {
    const env = Object.assign({}, process.env, { BRO_APP_HOME: root });
    const r = cp.spawnSync(exe, args, { encoding: 'utf8', timeout: 120000, env });
    const out = (r.stdout || '') + (r.stderr || '');
    assert(!/crash/.test(out), 'no crash: ' + args.join(' ') + '\n' + out.slice(-1500));
    return { status: r.status, out };
}

function waitFor(pred, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (pred()) return true;
        wallSleep(100);
    }
    return pred();
}

const records = () => (fs.existsSync(aot) ? fs.readdirSync(aot).filter((f) => f.endsWith('.bzm')) : []);

try {
    // The first run: compiled in process and harvested.
    const first = run([app, '--aot', aot]);
    assert(first.status === 0, 'the first run exits 0:\n' + first.out.slice(-1500));
    assert(first.out.includes('SERVER_AOT total=1198800'), 'the server script ran:\n' + first.out.slice(-1500));
    assert(!first.out.includes('SERVER_AOT_PAGE_RAN'), 'the page did not run');
    assert(/no ahead-of-time module for .*server\.js yet/.test(first.out), 'the script is harvested:\n' +
           first.out.slice(-1500));
    assert(/server\.js: its run is harvested/.test(first.out), 'its harvest was written:\n' + first.out.slice(-1500));

    // The build it started in the background.
    assert(waitFor(() => records().length === 1 && fs.existsSync(path.join(aot, 'aot.txt')) &&
                         /unit0=\S+ built/.test(fs.readFileSync(path.join(aot, 'aot.txt'), 'utf8')), 90000),
           'the background build made one module: ' + records() +
           (fs.existsSync(path.join(aot, 'aot.txt')) ? '\n' + fs.readFileSync(path.join(aot, 'aot.txt'), 'utf8') : ''));

    // The second run: from the module.
    const second = run([app, '--aot', aot]);
    assert(second.status === 0, 'the second run exits 0:\n' + second.out.slice(-1500));
    assert(second.out.includes('SERVER_AOT total=1198800'), 'the module ran the script:\n' + second.out.slice(-1500));
    assert(/server\.js runs its ahead-of-time module/.test(second.out), 'it ran from its module:\n' +
           second.out.slice(-1500));
    assert(!second.out.includes('to be harvested'), 'nothing is harvested again');

    // An explicit --aot-build of a current directory keeps the module.
    const rebuilt = run([app, path.join(app, 'server.js'), '--aot-build', aot]);
    assert(rebuilt.status === 0, 'the explicit build exits 0:\n' + rebuilt.out.slice(-1500));
    assert(/unit0=\S+ current/.test(fs.readFileSync(path.join(aot, 'aot.txt'), 'utf8')), 'the module is current:\n' +
           fs.readFileSync(path.join(aot, 'aot.txt'), 'utf8'));
} finally {
    fs.rmSync(root, { recursive: true, force: true });
}

console.log('test_server_aot: OK');
