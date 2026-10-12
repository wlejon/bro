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
// The default lists found: a machine without some of them (a list newer than its
// species folder) runs on the ones it has, and says so.
const wl = bro.vision && bro.vision.available !== false ? bro.vision.wildlife : null;
const loc = wl ? wl.locate() : null;
const foundLists = loc && Array.isArray(loc.species) ? loc.species.filter((s) => typeof s === 'string') : [];
const allLists = !!loc && foundLists.length === wl.SPECIES_LISTS.length;
const listOpts = allLists ? {} : { species: foundLists };
const skipWhy = !wl ? 'bro.vision is the stub'
    : loc.missing.includes('detector') || loc.missing.includes('classifier') || !foundLists.length
        ? 'wildlife models not found: missing ' + loc.missing.join(', ')
    : !wl.available(listOpts) ? 'wildlife models not available'
    : !bro.gpu || !bro.gpu.available ? 'no GPU backend' : null;
if (!skipWhy && !allLists) console.log('(default species lists not all found: running on ' + foundLists.length + ')');

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
    const an = wl.open(Object.assign({ cacheDir }, listOpts));
    assert(typeof an.device === 'string', 'device: ' + an.device);
    same([an.config.detector.repo, an.config.classifier.repo, an.config.threshold, an.config.maxClassify, an.config.candidates],
         ['google/owlv2-base-patch16-ensemble', 'imageomics/bioclip-2', 0.15, 8, { mode: 'all', petMinBoxScore: 0.3 }], 'config');
    same(an.config, wl.config(listOpts), 'open().config is config()');
    assert(an.config.speciesList.count === an.species.length && an.species.length > 100, 'the species list');
    // The default lists, merged: the birds and mammals, then the reptiles and
    // amphibians, the arthropods and the pets (those found).
    const names = ['upper-midwest.json', 'upper-midwest-herps/species.json', 'upper-midwest-arthropods/species.json', 'pets/species.json'];
    same(an.config.speciesList.files, loc.species.map((s, i) => s && names[i]).filter(Boolean), 'the default lists found');
    same(an.paths.species.length, foundLists.length, 'their files');
    assert(an.species.some((s) => s.class === 'Aves'), 'birds');
    if (loc.species[1]) {
        assert(an.species.some((s) => s.class === 'Amphibia') && an.species.some((s) => s.scientific === 'Thamnophis sirtalis'),
               'amphibians and the garter snake');
    }
    if (loc.species[2]) assert(an.species.some((s) => s.class === 'Insecta'), 'insects');
    same(new Set(an.species.map((s) => s.scientific.toLowerCase())).size, an.species.length, 'no species twice');
    assert(typeof an.taxa === 'object' && an.taxa !== null, 'the merged taxa map');
    assert(an.config.queries.includes('a snake') && an.config.queries.includes('a salamander'), 'the herp queries');
    for (const q of ['a dog', 'a cat', 'an insect', 'a dragonfly', 'a butterfly', 'a moth', 'a bee', 'a beetle', 'a spider']) {
        assert(an.config.queries.includes(q), 'the query ' + q);
    }
    assert(an.labelThreshold === 0.85 && an.label({ animal: true, species: [{ common: 'X', scientific: 'x', score: 0.9 }] }).common === 'X',
           'the analyzer labels at 0.85');
    same(an.labelOptions(), { labelThreshold: 0.85, taxa: an.taxa, minBoxScore: 0.3, broadMinBoxScore: 0.3, burstMinBoxScore: 0.4,
                              queryMatch: true, consensus: true }, 'and with the default box rule');
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
    // Every detection up to maxClassify is classified; the best one is the record's subject and species.
    // (An empty box, one a pixel row thin at threshold 0, is no animal.)
    const boxed = a.detections.filter((d) => d.box[2] > 0 && d.box[3] > 0);
    same([a.animals.length, a.animalsDropped], [Math.min(8, boxed.length), Math.max(0, boxed.length - 8)],
         'animals: up to 8, the rest counted');
    same([a.animals[0].box, a.animals[0].subject, a.animals[0].species], [boxed[0].box, a.subject, a.species],
         'the first animal is the best box');
    for (let i = 0; i < a.animals.length; i++) {
        const x = a.animals[i];
        same([x.box, x.label, x.score], [boxed[i].box, boxed[i].label, boxed[i].score], 'animal ' + i + ' is its box');
        assert(x.species.length === 5 && x.candidates === wl.candidatesFor(x.label, x.score), 'its top 5');
        assert(wl.RANKS.slice(1).every((r) => Array.isArray(x.taxa[r]) && x.taxa[r].length <= 3), 'its taxa');
        assert(x.taxa.class.length >= 1 && x.taxa.class[0].score >= x.species[0].score - 1e-4, 'a class scores at least its best species');
    }
    const few = an.analyze(frame(true), { source: src, threshold: 0, maxClassify: 2 });
    same([few.animals.length, few.animalsDropped], [2, boxed.length - 2], 'maxClassify');
    const bq = an.analyze(frame(true), { source: src, threshold: 0, maxClassify: 2, candidates: 'byQuery' });
    assert(bq.animals.every((x) => x.candidates === 'query' || x.candidates === 'fallback' || x.candidates === 'all'), 'byQuery per call');
    for (const x of bq.animals) {
        const cls = wl.queryClasses(x.label);
        if (x.candidates === 'query') assert(x.species.every((s) => cls.includes(s.class)), 'byQuery ranks the query\'s classes: ' + x.label);
    }
    // The pet rule per call: at petMinBoxScore 0 every dog or cat box ranks over the mammals.
    const pets = an.analyze(frame(true), { source: src, threshold: 0, petMinBoxScore: 0 });
    for (const x of pets.animals) {
        const pet = wl.PET_QUERIES.includes(x.label);
        assert(x.candidates === (pet ? 'query' : 'all'), 'petMinBoxScore 0: ' + x.label + ' ' + x.candidates);
        if (pet) assert(x.species.every((s) => s.class === 'Mammalia'), 'a pet box names mammals');
    }
    const noPets = an.analyze(frame(true), { source: src, threshold: 0, petMinBoxScore: false });
    assert(noPets.animals.every((x) => x.candidates === 'all'), 'petMinBoxScore false: every box over every row');
    assert(Array.isArray(an.labels(a)), 'analyzer labels()');

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
    const again = wl.open(Object.assign({ cacheDir, minBoxScore: 0.5, queryMatch: false }, listOpts));
    assert(again.cached === true, 'second open reads the cache');
    same([again.labelOptions().minBoxScore, again.labelOptions().queryMatch, again.labelOptions().burstMinBoxScore],
         [0.5, false, 0.4], 'open() takes the box rule');
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
