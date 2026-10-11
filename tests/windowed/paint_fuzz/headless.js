// The paint cache fuzz, headless (run by tests/layout/test_paint_cache_fuzz.js
// in a child bro-headless on this app, PAINT_FUZZ_MODE=headless). After each
// seeded step: the frame with the span cache and partial raster, against the
// same frame recorded and rastered whole (setPaintCache(false)).
//
// Each step is captured twice. A capture alternates between two pools of
// surfaces, as the windowed raster thread does, and each pool redraws the
// damage since the recording it holds: the first capture after a step holds
// the recording from two captures ago, the second (nothing changed in
// between) the one from just before the step, so it needs the step's damage
// from the history. A pass that dropped an earlier generation's damage shows
// the step's change stale there, on every seed.
//
// Frames differ when an area of pixels does (steps.js: diff): a partial
// raster antialiases an edge pixel a little differently from a whole one,
// which is reported (edgePixels) and passes; stale content is an area.
//
// PAINT_FUZZ_SEED picks the seed, PAINT_FUZZ_STEPS how many steps; the seed
// is printed with any failure, so it replays.

const seed = Number(process.env.PAINT_FUZZ_SEED || 1) >>> 0;
const STEPS = Number(process.env.PAINT_FUZZ_STEPS || 150);
const fuzz = PaintFuzz.create(seed);
fuzz.build();
flush();
setPaintCache(true);
frameImage();   // the first frames record every span
frameImage();

const history = [];
let failed = false;
let edgePixels = 0;  // apart along an edge only: antialiasing under a partial raster (steps.js)
const edgeSteps = [];  // the captures they were in, to name the most
for (let i = 0; i < STEPS && !failed; ++i) {
    const s = fuzz.step();
    history.push(i + ':' + s.what);
    if (s.hover) mouseMove(s.hover[0], s.hover[1]);
    if (s.dpr) setDeviceScaleFactor(s.dpr);
    if (s.advance) advanceTime(s.advance);
    flush();
    const cached = [frameImage(), frameImage()];
    setPaintCache(false);
    const full = frameImage();
    setPaintCache(true);
    for (let k = 0; k < 2 && !failed; ++k) {
        const d = PaintFuzz.diff(cached[k], full);
        edgePixels += d.edge;
        if (d.edge) edgeSteps.push({ step: i, what: s.what, capture: k + 1, edge: d.edge, at: d.edgeBounds });
        if (d.count !== 0) {
            failed = true;
            assert(false, `paint cache fuzz seed=${seed}: step ${i} (${s.what}), capture ${k + 1} — ${d.count} pixels ` +
                          `differ from a full re-record, first at ${JSON.stringify(d.first)} (cached, full); last steps: ` +
                          history.slice(-8).join(' | '));
        }
    }
}
const ps = perf.stats().paint;
console.log(`PAINT_FUZZ seed=${seed} steps=${history.length} ${failed ? 'FAILED' : 'ok'} edgePixels=${edgePixels} reused=${ps.reused} ` +
            `recorded=${ps.recorded} misses=${JSON.stringify(ps.misses)}`);
// The captures with the most edge pixels, with where they were (x0,y0,x1,y1):
// a count that creeps up has a step and a place to look at.
edgeSteps.sort((a, b) => b.edge - a.edge);
if (edgeSteps.length)
    console.log(`PAINT_FUZZ_EDGES seed=${seed} captures=${edgeSteps.length} most: ` +
                edgeSteps.slice(0, 3).map((e) => `step ${e.step} (${e.what}) capture ${e.capture}: ${e.edge} at ` +
                                                 `[${e.at.join(',')}]`).join('; '));
assert(failed || ps.reused > 0, `paint cache fuzz seed=${seed}: no span was ever reused, so nothing was tested`);
