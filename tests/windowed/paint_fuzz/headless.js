// The paint cache fuzz, headless (run by tests/layout/test_paint_cache_fuzz.js
// in a child bro-headless on this app, PAINT_FUZZ_MODE=headless). After each
// seeded step: the frame with the span cache, against the same frame with it
// off (every span re-recorded). PAINT_FUZZ_SEED picks the seed, PAINT_FUZZ_STEPS
// how many steps; the seed is printed with any failure, so it replays.

const seed = Number(process.env.PAINT_FUZZ_SEED || 1) >>> 0;
const STEPS = Number(process.env.PAINT_FUZZ_STEPS || 150);
const fuzz = PaintFuzz.create(seed);
fuzz.build();
flush();
setPaintCache(true);
frameImage();   // the first frame records every span

const history = [];
let failed = false;
let edgePixels = 0;  // apart along an edge only: antialiasing under a damage clip (steps.js)
for (let i = 0; i < STEPS && !failed; ++i) {
    const s = fuzz.step();
    history.push(i + ':' + s.what);
    if (s.hover) mouseMove(s.hover[0], s.hover[1]);
    if (s.dpr) setDeviceScaleFactor(s.dpr);
    if (s.advance) advanceTime(s.advance);
    flush();
    const cached = frameImage();
    setPaintCache(false);
    const full = frameImage();
    setPaintCache(true);
    const d = PaintFuzz.diff(cached, full);
    edgePixels += d.edge;
    if (d.count !== 0) {
        failed = true;
        assert(false, `paint cache fuzz seed=${seed}: step ${i} (${s.what}) — ${d.count} pixels differ from a full ` +
                      `re-record, first at ${JSON.stringify(d.first)} (cached, full); last steps: ` +
                      history.slice(-8).join(' | '));
    }
}
const ps = perf.stats().paint;
console.log(`PAINT_FUZZ seed=${seed} steps=${history.length} ${failed ? 'FAILED' : 'ok'} edgePixels=${edgePixels} reused=${ps.reused} ` +
            `recorded=${ps.recorded} misses=${JSON.stringify(ps.misses)}`);
assert(failed || ps.reused > 0, `paint cache fuzz seed=${seed}: no span was ever reused, so nothing was tested`);
