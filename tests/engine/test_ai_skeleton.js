// system/skeletons/ai — the AI-chat starter the project manager seeds new
// projects from. Loaded headless as its own app (child bro-headless, same
// pattern as test_location_reload_toplevel.js), it must come up cleanly in
// the no-model state: GPU probe rendered, composer disabled, and a clear
// "no model loaded" message — WITHOUT touching the Load-model button (it
// opens a native file dialog, which blocks even in headless).

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', '..',
                         'system', 'skeletons', 'ai');
const verify = path.join(process.env.BRO_APP_DIR, '..', 'engine',
                         'ai_skeleton_verify.js');

const r = cp.spawnSync(exe, [appDir, verify], { encoding: 'utf8' });

const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'child bro-headless exited ' + r.status + '\n--- child output ---\n' + out);
assert(out.includes('AI_SKELETON_OK'),
       'skeleton came up in the no-model state; output was:\n' + out);

console.log('ai skeleton smoke OK');
