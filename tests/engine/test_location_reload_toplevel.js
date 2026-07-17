// Top-level location.reload(): tears down the app document + its JS realm
// and re-parses/re-runs the app in the same Engine and window.
//
// Why a child process: in headless mode the driving script runs INSIDE the
// app realm, so it cannot itself survive the reload it wants to observe. The
// reload is therefore drained between the driver's evaluation units — and
// observing it takes a second bro-headless whose APP reloads itself during
// its first run, with the post-reload realm asserted by a follow-up script
// (reload_app/verify.js) that the driver evaluates in the SECOND realm.
//
// What the child proves: fresh globals (a canary planted by run 1 is gone),
// scripts re-executed against a fresh DOM, readyState complete, run-1 timers
// dead, and the fresh realm's timers working. Any failed assert exits the
// child nonzero.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'engine', 'reload_app');
const verify = path.join(appDir, 'verify.js');

const r = cp.spawnSync(exe, [appDir, verify], { encoding: 'utf8' });

const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'child bro-headless exited ' + r.status + '\n--- child output ---\n' + out);
assert(out.includes('TOPLEVEL_RELOAD_OK'),
       'child observed the reload; output was:\n' + out);

console.log('top-level location.reload OK');
