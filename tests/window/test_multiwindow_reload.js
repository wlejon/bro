// Secondary windows are torn down with the app realm on location.reload().
//
// Why a child process: in headless the driving script runs INSIDE the app
// realm, so it cannot survive the reload it wants to observe (same reasoning
// as tests/engine/test_location_reload_toplevel.js). The child app opens two
// secondary windows, brings both to a fully loaded + rendered state, reloads
// itself, and then proves the fresh realm can open and render a window again.
//
// What this covers that the in-realm test can't: performAppReload destroying
// live host documents, their canvas scenes, and their GPU surfaces — the path
// where an unrouted surface leaks or a stale JSContext faults.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'window', 'multiwin_reload_app');

const r = cp.spawnSync(exe, [appDir], { encoding: 'utf8' });

const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'child bro-headless exited ' + r.status + '\n--- child output ---\n' + out);
assert(out.includes('MULTIWIN_RELOAD_OK'),
       'child reopened + rendered a window after reload; output was:\n' + out);

console.log('multiwindow reload teardown OK');
