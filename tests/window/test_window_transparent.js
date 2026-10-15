// A transparent, click-through window (bro.json "transparent" / "clickThrough"):
// its captures carry straight alpha, and its input region is what the page's
// hit test takes. The keys are read at Engine construction, so a second
// bro-headless runs transparent_app/, whose verify.js asserts it all.
// (The Windows / Wayland / macOS halves, the desktop showing through and
// input reaching other applications, need a live desktop session.)

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'window', 'transparent_app');
const verify = path.join(appDir, 'verify.js');

const r = cp.spawnSync(exe, [appDir, verify], { encoding: 'utf8', timeout: 60000 });

const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'child bro-headless exited ' + r.status + '\n--- child output ---\n' + out);
assert(out.includes('TRANSPARENT_WINDOW_OK'),
       'child verified the transparent window; output was:\n' + out);

console.log('transparent window OK');
