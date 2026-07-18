// bro.json window-management manifest keys (borderless, alwaysOnTop,
// minWidth/minHeight, maxWidth/maxHeight) parse and apply at startup.
//
// Why a child process: the manifest is consumed at Engine construction, so a
// second bro-headless is launched with manifest_app/ (which carries the keys)
// and its own verify.js asserts the resulting window state. windowX/windowY/
// display are positioning keys — skipped when the window is hidden — so they
// need windowed manual verification and are not asserted here.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'window', 'manifest_app');
const verify = path.join(appDir, 'verify.js');

const r = cp.spawnSync(exe, [appDir, verify], { encoding: 'utf8' });

const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'child bro-headless exited ' + r.status + '\n--- child output ---\n' + out);
assert(out.includes('MANIFEST_WINDOW_OK'),
       'child verified manifest keys; output was:\n' + out);

console.log('manifest window keys OK');
