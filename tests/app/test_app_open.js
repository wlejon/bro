// bro.app.open(id, args) / bro.app.find(id): another folder app by id, found
// where `bro <id>` finds it (the install roots of `bro --install`, docs/apps.md),
// else beside this app in its project, and started with the stock bro:
// `bro <dir> args...`. Headless starts nothing: it records the call, which
// openedApps() reports, so the resolution and the command line are what is
// tested here (a single-instance app's hand-off is bro's own, covered by
// test_app_single_instance.js).

const fs = require('fs');
const path = require('path');
const os = require('os');

const scratch = path.join(os.tmpdir(), 'bro_open_test_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });

// The install roots, redirected into the scratch dir (bro reads them from the
// environment at each call).
const saved = {};
for (const k of ['LOCALAPPDATA', 'ProgramFiles', 'HOME', 'XDG_DATA_HOME', 'XDG_DATA_DIRS', 'BRO_PROJECT_ROOT'])
    saved[k] = process.env[k];
let root;
if (process.platform === 'win32') {
    process.env.LOCALAPPDATA = path.join(scratch, 'local');
    process.env.ProgramFiles = path.join(scratch, 'pf');
    root = path.join(scratch, 'local', 'bro', 'apps');
} else {
    process.env.HOME = path.join(scratch, 'home');
    process.env.XDG_DATA_HOME = path.join(scratch, 'data');
    process.env.XDG_DATA_DIRS = path.join(scratch, 'none');
    root = path.join(scratch, 'data', 'bro', 'apps');
}

const id = 'org.bro.test.Opened' + (process.pid % 100000);
const installed = path.join(root, id);
fs.mkdirSync(installed, { recursive: true });
fs.writeFileSync(path.join(installed, 'bro.json'), JSON.stringify({ id, name: 'Opened', singleInstance: true }));
fs.writeFileSync(path.join(installed, 'index.html'), '<!DOCTYPE html><html><body>opened</body></html>');

// A project sibling: found by the id its bro.json declares.
const project = path.join(scratch, 'project');
const siblingId = 'org.bro.test.Sibling' + (process.pid % 100000);
fs.mkdirSync(path.join(project, 'sib'), { recursive: true });
fs.writeFileSync(path.join(project, 'sib', 'bro.json'), JSON.stringify({ id: siblingId, name: 'Sib' }));
fs.writeFileSync(path.join(project, 'sib', 'index.html'), '<!DOCTYPE html><html><body>sib</body></html>');
process.env.BRO_PROJECT_ROOT = project;

const same = (a, b) => path.resolve(a).toLowerCase() === path.resolve(b).toLowerCase();

assert(typeof bro.app.open === 'function' && typeof bro.app.find === 'function', 'bro.app.open and find exist');
assert(typeof openedApps === 'function', 'headless openedApps()');
openedApps({ clear: true });

// find: where open would look.
const f = bro.app.find(id);
assert(f && f.from === 'installed' && same(f.dir, installed), 'find: the installed app: ' + JSON.stringify(f));
const s = bro.app.find(siblingId);
assert(s && s.from === 'project' && same(s.dir, path.join(project, 'sib')), 'find: the project sibling: ' + JSON.stringify(s));
assert(bro.app.find('org.bro.test.NoSuchApp') === null, 'find: an unknown id is null');
assert(bro.app.find('../escape') === null, 'find: a path is not an id');

let result = null, failure = null;
bro.app.open(id, ['--file', 'two words.txt']).then((r) => { result = r; }, (e) => { failure = e; });
advanceTime(16);
assert(!failure, 'open resolved: ' + failure);
assert(result && result.id === id && result.from === 'installed', 'open: the installed app: ' + JSON.stringify(result));
assert(same(result.dir, installed), 'open: its folder');
assert(result.spawned === false, 'headless starts nothing');
const cmd = result.command;
assert(Array.isArray(cmd) && cmd.length === 4, 'command is bro, the folder, the args: ' + JSON.stringify(cmd));
const stock = path.join(path.dirname(process.execPath), process.platform === 'win32' ? 'bro.exe' : 'bro');
if (fs.existsSync(stock))
    assert(same(cmd[0], stock), 'the stock bro beside bro-headless: ' + cmd[0]);
assert(same(cmd[1], installed), 'then the folder: ' + cmd[1]);
assert(cmd[2] === '--file' && cmd[3] === 'two words.txt', 'then the args, verbatim');
assert(JSON.stringify(result.args) === '["--file","two words.txt"]', 'args: ' + JSON.stringify(result.args));
assert(typeof result.cwd === 'string' && result.cwd.length > 0, 'the working directory it would start in');

result = null;
bro.app.open(siblingId, [], { newInstance: true }).then((r) => { result = r; }, (e) => { failure = e; });
advanceTime(16);
assert(result && result.from === 'project' && result.command[1] === '--new-instance' &&
       same(result.command[2], path.join(project, 'sib')) && result.args.length === 0,
    'open: a project sibling, as a new instance: ' + JSON.stringify(result));

let rejected = null;
bro.app.open('org.bro.test.NoSuchApp').then(() => { rejected = 'resolved'; }, (e) => { rejected = e; });
advanceTime(16);
assert(rejected instanceof Error && /no app/.test(rejected.message), 'an unknown id rejects: ' + rejected);

const opened = openedApps();
assert(opened.length === 2 && opened[0].id === id && opened[1].id === siblingId,
    'openedApps() lists both calls: ' + JSON.stringify(opened));
openedApps({ clear: true });
assert(openedApps().length === 0, 'and clears');

for (const k in saved) {
    if (saved[k] === undefined) delete process.env[k];
    else process.env[k] = saved[k];
}
try { fs.rmSync(scratch, { recursive: true, force: true }); } catch (e) { /* best effort */ }
console.log('test_app_open.js PASSED');
