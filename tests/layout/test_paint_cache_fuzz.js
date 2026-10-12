// The paint span cache (layout/paint_cache.h) against a full re-record: a
// seeded fuzz of DOM, style, layout, scroll, hover, transition, animation,
// font and devicePixelRatio steps, comparing after every step the frame the
// cache produced with the frame recorded from scratch. The fuzz app
// (tests/windowed/paint_fuzz, which also runs windowed) declares its own
// @font-face, so it runs in a child bro-headless with its headless.js.
//
// Two fixed seeds, and one new one each run (printed): a failure names its
// seed, and PAINT_FUZZ_SEED=<seed> replays just that one. The seeds that broke
// a scroll layer's partial raster on the CPU are test_partial_raster_seed_*.js.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const app = path.join(process.env.BRO_APP_DIR, '..', 'windowed', 'paint_fuzz');
const driver = path.join(app, 'headless.js');

// And one with the scroller's rows under rounded clips (PAINT_FUZZ_ROUNDED:
// a scroll layer draws them as painting in place does).
const seeds = process.env.PAINT_FUZZ_SEED
    ? [[Number(process.env.PAINT_FUZZ_SEED) >>> 0, process.env.PAINT_FUZZ_ROUNDED === '1']]
    : [[1, false], [2, false], [(Date.now() ^ Math.floor(Math.random() * 0x7fffffff)) >>> 0, false], [3, true]];
// 60 steps a seed keeps the run near a minute; PAINT_FUZZ_STEPS runs longer.
const steps = process.env.PAINT_FUZZ_STEPS || '60';

for (const [seed, rounded] of seeds) {
    const env = Object.assign({}, process.env, {
        PAINT_FUZZ_MODE: 'headless', PAINT_FUZZ_SEED: String(seed), PAINT_FUZZ_STEPS: steps,
        PAINT_FUZZ_ROUNDED: rounded ? '1' : '0',
    });
    const r = cp.spawnSync(exe, [app, driver], { encoding: 'utf8', env });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.includes('PAINT_FUZZ seed='));
    assert(r.status === 0,
           `paint cache fuzz seed=${seed} failed (exit ${r.status}); replay with PAINT_FUZZ_SEED=${seed}` +
           `${rounded ? ' PAINT_FUZZ_ROUNDED=1' : ''}\n` +
           out.split(/\r?\n/).filter((l) => /ASSERT|PAINT_FUZZ|ERROR/.test(l)).join('\n'));
    assert(line && line.includes(' ok '), `paint cache fuzz seed=${seed} ran to the end:\n${out}`);
    console.log(line.slice(line.indexOf('PAINT_FUZZ')));
    const edges = out.split(/\r?\n/).find((l) => l.includes('PAINT_FUZZ_EDGES'));
    if (edges) console.log(edges.slice(edges.indexOf('PAINT_FUZZ_EDGES')));
}