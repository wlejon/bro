// The paint span cache (layout/paint_cache.h) against a full re-record: a
// seeded fuzz of DOM, style, layout, scroll, hover, transition, animation,
// font and devicePixelRatio steps, comparing after every step the frame the
// cache produced with the frame recorded from scratch. The fuzz app
// (tests/windowed/paint_fuzz, which also runs windowed) declares its own
// @font-face, so it runs in a child bro-headless with its headless.js.
//
// Two fixed seeds, and one new one each run (printed): a failure names its
// seed, and PAINT_FUZZ_SEED=<seed> replays just that one.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const app = path.join(process.env.BRO_APP_DIR, '..', 'windowed', 'paint_fuzz');
const driver = path.join(app, 'headless.js');

const seeds = process.env.PAINT_FUZZ_SEED
    ? [Number(process.env.PAINT_FUZZ_SEED) >>> 0]
    : [1, 2, (Date.now() ^ Math.floor(Math.random() * 0x7fffffff)) >>> 0];
// 60 steps a seed keeps the run near a minute; PAINT_FUZZ_STEPS runs longer.
const steps = process.env.PAINT_FUZZ_STEPS || '60';

for (const seed of seeds) {
    const env = Object.assign({}, process.env, {
        PAINT_FUZZ_MODE: 'headless', PAINT_FUZZ_SEED: String(seed), PAINT_FUZZ_STEPS: steps,
    });
    const r = cp.spawnSync(exe, [app, driver], { encoding: 'utf8', env });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.includes('PAINT_FUZZ seed='));
    assert(r.status === 0,
           `paint cache fuzz seed=${seed} failed (exit ${r.status}); replay with PAINT_FUZZ_SEED=${seed}\n` +
           out.split(/\r?\n/).filter((l) => /ASSERT|PAINT_FUZZ|ERROR/.test(l)).join('\n'));
    assert(line && line.includes(' ok '), `paint cache fuzz seed=${seed} ran to the end:\n${out}`);
    console.log(line.slice(line.indexOf('PAINT_FUZZ')));
    const edges = out.split(/\r?\n/).find((l) => l.includes('PAINT_FUZZ_EDGES'));
    if (edges) console.log(edges.slice(edges.indexOf('PAINT_FUZZ_EDGES')));
}
