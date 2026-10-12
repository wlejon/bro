// require('./x') in a script bro-headless runs resolves against the script's
// own directory (docs/brokit-api.js: the file that calls require, as in
// Node), not the cwd. A script run as `bro-headless <app> dir/script.js` from
// a cwd elsewhere threw "Cannot find module './helper'": bro ran it with no
// require directory, and require fell back to the cwd.

const cp = require('child_process');
const path = require('path');

const script = path.join(process.env.BRO_APP_DIR, '..', 'brokit', 'require_dir', 'script.js');
const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
// The repository root: neither the script's directory nor the app's (and no
// symlink on the way, as macOS's temp directory has, for the relative form).
const cwd = path.resolve(process.env.BRO_APP_DIR, '..', '..');
assert(path.resolve(cwd) !== path.dirname(path.resolve(script)), 'the cwd is elsewhere');

for (const form of ['absolute', 'relative']) {
    const arg = form === 'absolute' ? script : path.relative(cwd, script);
    const r = cp.spawnSync(exe, [process.env.BRO_APP_DIR, arg], { encoding: 'utf8', env: process.env, cwd });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.includes('REQUIRE_DIR'));
    assert(r.status === 0 && line && line.includes('require_dir/helper.js require_dir/data.json'),
           `${form} script path: its siblings resolve (exit ${r.status}): ` +
           out.split(/\r?\n/).filter((l) => /REQUIRE_DIR|ERROR|Cannot find/.test(l)).join(' | '));
}
console.log('require from a script resolves against the script\'s directory');
