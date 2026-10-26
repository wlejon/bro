// One seed of the paint fuzz (tests/windowed/paint_fuzz) in a child
// bro-headless --no-gpu with the partial raster check on (headless
// setScrollLayerVerify, PAINT_FUZZ_VERIFY_PARTIAL): every scroll layer
// rastered in place where only part of it changed is its whole raster, pixel
// for pixel. For test_partial_raster_seed_*.js, one seed each (a CPU run of
// the fuzz takes about 1.4 s a step).

const cp = require('child_process');
const path = require('path');

module.exports = function checkPartialRasterSeed(seed, steps) {
    const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
    const exe = path.join(process.env.BRO_EXE_DIR, exeName);
    const app = path.join(process.env.BRO_APP_DIR, '..', 'windowed', 'paint_fuzz');
    const env = Object.assign({}, process.env, {
        PAINT_FUZZ_MODE: 'headless', PAINT_FUZZ_SEED: String(seed), PAINT_FUZZ_STEPS: String(steps),
        PAINT_FUZZ_VERIFY_PARTIAL: '1',
    });
    // --grant env: the app reads PAINT_FUZZ_* from process.env, which shows
    // an app without `env` only who and where it is (docs/apps.md).
    const r = cp.spawnSync(exe, ['--no-gpu', '--grant', 'env', app, path.join(app, 'headless.js')],
                           { encoding: 'utf8', env });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.includes('PAINT_FUZZ seed='));
    assert(r.status === 0 && line && line.includes(' ok '),
           `partial raster check seed=${seed} failed (exit ${r.status}); replay with bro-headless --no-gpu and ` +
           `PAINT_FUZZ_MODE=headless PAINT_FUZZ_VERIFY_PARTIAL=1 PAINT_FUZZ_SEED=${seed} PAINT_FUZZ_STEPS=${steps}\n` +
           out.split(/\r?\n/).filter((l) => /ASSERT|PAINT_FUZZ|ERROR/.test(l)).join('\n'));
    if (line) console.log(line.slice(line.indexOf('PAINT_FUZZ')).replace(/ misses=.*/, ''));
};
