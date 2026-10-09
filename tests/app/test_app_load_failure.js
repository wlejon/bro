// An app that cannot load ends the run with a clean error, never a crash:
// bro-headless on a path that does not exist says so and exits 1, and on a
// directory with nothing to load (no index.html, no scripts) the Engine's
// constructor throws and must tear down what it had started — Skia's
// context, the renderer drawing with it, the Vulkan device — in the order
// ~Engine keeps, not member destruction order (which took the GPU context
// before the renderer and aborted on macOS: "recursive_mutex lock failed").

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

const scratch = path.join(os.tmpdir(), 'bro_load_failure_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });

function run(appDir) {
    const r = cp.spawnSync(headless, [appDir, '-e', '1'], { encoding: 'utf8', timeout: 60000 });
    const err = (r.stderr || '') + (r.stdout || '');
    assert(!/crash|recursive_mutex|terminate|Abort|SIGABRT|SIGSEGV/i.test(err),
        appDir + ': no crash on the way out: ' + err.slice(-800));
    assert(r.status === 1, appDir + ': exits 1 (status ' + r.status + ', signal ' + r.signal + ')');
    return err;
}

// Nothing at the path.
const missing = path.join(scratch, 'does-not-exist');
const out = run(missing);
assert(/no app directory/.test(out), 'it names the missing app: ' + out.slice(-400));

// A directory with nothing to load: the Engine is built and fails.
const empty = path.join(scratch, 'empty');
fs.mkdirSync(empty);
const out2 = run(empty);
assert(/Failed to load index\.html/.test(out2), 'it says why: ' + out2.slice(-400));

fs.rmSync(scratch, { recursive: true, force: true });
