// bro.vision.wildlife with its models (OWLv2 + BioCLIP 2 over the species
// list, found through bro.models: owlv2/, bioclip-2/, species/upper-midwest.json
// under BRO_MODELS). Skips when they are not there. A synthetic frame: the
// default threshold finds nothing in an empty one; at threshold 0 a frame
// with a blob gets boxes, a square subject inside the source frame and a
// sorted top 5; the same image analyzes to the same record twice; the text
// embeddings cache round-trips; dispose() ends it.

const fs = require('fs');
const os = require('os');
const path = require('path');

// skipTest() does not stop the script, so the checks run only without a reason to skip.
const wl = bro.vision && bro.vision.available !== false ? bro.vision.wildlife : null;
const skipWhy = !wl ? 'bro.vision is the stub'
    : !wl.available() ? 'wildlife models not found: missing ' + wl.locate().missing.join(', ')
    : !bro.gpu || !bro.gpu.available ? 'no GPU backend' : null;

const same = (a, b, what) => assert(JSON.stringify(a) === JSON.stringify(b),
    what + ': ' + JSON.stringify(a) + ' vs ' + JSON.stringify(b));

// 640 x 400 RGB: a sky-to-grass gradient; with `blob`, a dark brown ellipse.
function frame(blob) {
    const w = 640, h = 400, px = new Uint8Array(w * h * 3);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 3;
        const inside = blob && ((x - 420) / 60) ** 2 + ((y - 220) / 40) ** 2 < 1;
        px[i] = inside ? 90 : 120 - (y >> 3);
        px[i + 1] = inside ? 60 : 160 + (y >> 4);
        px[i + 2] = inside ? 40 : 220 - (y >> 1);
    }
    return { width: w, height: h, channels: 3, pixels: px };
}

function run() {
    const cacheDir = path.join(os.tmpdir(), `bro-wildlife-cache-${process.pid}-${Date.now()}`);
    const an = wl.open({ cacheDir });
    assert(typeof an.device === 'string', 'device: ' + an.device);
    same([an.config.detector.repo, an.config.classifier.repo, an.config.threshold],
         ['google/owlv2-base-patch16-ensemble', 'imageomics/bioclip-2', 0.15], 'config');
    same(an.config, wl.config(), 'open().config is config()');
    assert(an.config.speciesList.count === an.species.length && an.species.length > 100, 'the species list');
    assert(an.cached === false, 'first open embeds the list');

    // Nothing in an empty frame at the default threshold.
    const e = an.analyze(frame(false));
    same([e.animal, e.detections, e.subject, e.species], [false, [], null, []], 'empty frame: no animal');

    // Threshold 0: every frame has a box; the record is in source pixels (2x).
    const src = { width: 1280, height: 800 };
    const a = an.analyze(frame(true), { source: src, threshold: 0 });
    assert(a.animal === true && a.detections.length >= 1 && a.detections.length <= 20, 'boxes: ' + a.detections.length);
    for (let i = 0; i < a.detections.length; i++) {
        const d = a.detections[i];
        assert(wl.QUERIES.includes(d.label), 'label is a query: ' + d.label);
        assert(d.box[0] >= 0 && d.box[1] >= 0 && d.box[0] + d.box[2] <= 1280.1 && d.box[1] + d.box[3] <= 800.1, 'box in the source');
        if (i) assert(a.detections[i - 1].score >= d.score, 'best first');
    }
    const s = a.subject;
    assert(s && s.w === s.h && s.x >= 0 && s.y >= 0 && s.x + s.w <= 1280 && s.y + s.h <= 800, 'subject square: ' + JSON.stringify(s));
    assert(a.species.length === 5 && a.species.every((x) => x.common && x.scientific && x.score >= 0 && x.score <= 1), 'top 5');
    for (let i = 1; i < 5; i++) assert(a.species[i - 1].similarity >= a.species[i].similarity, 'ranked by similarity');
    assert(typeof a.timing.detect === 'number' && typeof a.timing.classify === 'number', 'timing');

    // Deterministic: the same image, the same record.
    const b = an.analyze(frame(true), { source: src, threshold: 0 });
    delete a.timing; delete b.timing;
    same(b, a, 'a second analyze');

    // detect() / classify(): the raw halves, in the image's pixels.
    const hits = an.detect(frame(true), { threshold: 0, maxDetections: 3, queries: ['a rock'] });
    assert(hits.length === 3 && hits.every((h) => h.label === 'a rock'), 'custom queries');
    // RGBA rows give the boxes RGB rows do.
    const rgb = frame(true), rgbaPx = new Uint8Array(640 * 400 * 4);
    bro.image.rgbToRgba(rgbaPx, rgb.pixels, 640 * 400, 255);
    same(an.detect({ width: 640, height: 400, data: rgbaPx }, { threshold: 0, maxDetections: 3, queries: ['a rock'] })
             .map((r) => r.box.map(Math.round)), hits.map((r) => r.box.map(Math.round)), 'RGB and RGBA rows');
    const top = an.classify(frame(true), { topK: 3 });
    assert(top.length === 3, 'classify topK');

    // The text embeddings come back from the cache unchanged.
    const again = wl.open({ cacheDir });
    assert(again.cached === true, 'second open reads the cache');
    const c = again.analyze(frame(true), { source: src, threshold: 0 });
    delete c.timing;
    same(c, a, 'the cached list ranks the same');
    again.dispose();

    an.dispose();
    let threw = false;
    try { an.analyze(frame(false)); } catch (_) { threw = true; }
    assert(threw, 'analyze after dispose throws');

    try { fs.rmSync(cacheDir, { recursive: true, force: true }); } catch (_) {}
    console.log('bro.vision.wildlife models OK');
}

if (skipWhy) skipTest(skipWhy);
else run();
