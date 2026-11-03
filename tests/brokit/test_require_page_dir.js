// require('./x') from a page's scripts resolves where docs/brokit-api.js
// says, not against the cwd. The app (require_page/) runs from a cwd
// elsewhere, as bro-headless <app> from the repository root does:
// - an inline classic script and a timer it leaves resolve against the app's
//   folder. They have no file of their own, and threw "Cannot find module"
//   with brokit's fallback to the cwd;
// - a module, when a function of it requires after it loaded, resolves
//   against the module's own folder (each module gets its own require);
// - a module script file (<script type="module" src="lib/m.js">) resolves
//   against its own folder.

const cp = require('child_process');
const path = require('path');

const app = path.join(process.env.BRO_APP_DIR, '..', 'brokit', 'require_page');
const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const cwd = path.resolve(process.env.BRO_APP_DIR, '..', '..');
assert(path.resolve(cwd) !== path.resolve(app), 'the cwd is elsewhere');

const r = cp.spawnSync(exe, ['--grant', 'files', app, '-e', 'advanceTime(50); "REQUIRE_PAGE " + JSON.stringify(window.REQ)'],
                       { encoding: 'utf8', env: process.env, cwd });
const out = (r.stdout || '') + (r.stderr || '');
const line = out.split(/\r?\n/).find((l) => l.includes('REQUIRE_PAGE {'));
const got = line ? JSON.parse(line.slice(line.indexOf('{'))) : null;
assert(r.status === 0 && got, `the page ran (exit ${r.status}): ` +
       out.split(/\r?\n/).filter((l) => /REQUIRE_PAGE|ERROR|Cannot find/.test(l)).join(' | '));
assert(got.inline === 'a', `an inline script requires from the app's folder: ${JSON.stringify(got)}`);
assert(got.timer === 'b', `a timer the page left requires from the app's folder: ${JSON.stringify(got)}`);
assert(got.lazy === 'b', `a module's later require resolves from its own folder: ${JSON.stringify(got)}`);
assert(got.module === 'b', `a module script file requires from its own folder: ${JSON.stringify(got)}`);
console.log('require from a page resolves against the app and the calling file');
