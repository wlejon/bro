// bro.vision.wildlife without weights (docs/vision-api.js, "Wildlife"): the
// surface in the main realm and in a Worker, available() / locate() / open()
// on an empty model directory, config() without loading, the pixel helpers,
// and burst grouping on synthetic records (gap, consensus, keeper, scene
// split). tests/vision/test_vision_wildlife_models.js runs the models.

const fs = require('fs');
const os = require('os');
const path = require('path');

// skipTest() does not stop the script, so the checks run only without a reason to skip
// (an app-profile build has bro.vision as the stub, and no bro.vision.wildlife).
const why = !bro.vision || bro.vision.available === false ? 'bro.vision is the stub'
    : !bro.vision.wildlife ? 'this build has no bro.vision.wildlife' : null;
const wl = why ? null : bro.vision.wildlife;

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}
const same = (a, b, what) => assert(JSON.stringify(a) === JSON.stringify(b),
    what + ': ' + JSON.stringify(a) + ' vs ' + JSON.stringify(b));

function run() {
// ── surface ──────────────────────────────────────────────────────────────
assert(typeof wl === 'object' && wl !== null, 'bro.vision.wildlife exists');
for (const f of ['open', 'available', 'locate', 'config', 'subjectSquare', 'cropImage', 'squareInput',
                 'sharpness', 'speciesPrompt', 'readSpeciesList', 'readSpecies', 'label', 'parseExifTime',
                 'keeperScore', 'groupBursts']) {
    assert(typeof wl[f] === 'function', 'wildlife.' + f + ' is a function');
}
assert(wl.QUERIES.length === 19 && wl.QUERIES[0] === 'a bird' && wl.QUERIES[12] === 'a mink', 'the animal queries');
same(wl.QUERIES.slice(13), ['a snake', 'a turtle', 'a frog', 'a toad', 'a lizard', 'a salamander'],
     'then the reptiles and amphibians');
same([wl.DEFAULTS.threshold, wl.DEFAULTS.topK, wl.DEFAULTS.margin, wl.DEFAULTS.gapS, wl.DEFAULTS.model, wl.DEFAULTS.labelThreshold],
     [0.15, 5, 0.15, 1, 'bioclip-2', 0.85], 'defaults');
same(wl.SPECIES_LISTS, [{ folder: 'species', file: 'upper-midwest.json' },
                        { folder: 'species/upper-midwest-herps', file: 'species.json' }], 'the default species lists');

// ── an empty model directory: not available, and open() says why ────────
const root = path.join(os.tmpdir(), `bro-wildlife-${process.pid}-${Date.now()}`);
const empty = path.join(root, 'empty');
fs.mkdirSync(empty, { recursive: true });
const none = { detector: empty, classifier: empty, species: path.join(empty, 'list.json') };
assert(wl.available(none) === false, 'available() is false on an empty directory');
same(wl.locate(none).missing, ['detector', 'classifier', 'species'], 'locate() names what is missing');
let err = expectThrows(() => wl.open(none), 'open() on an empty directory');
assert(err.code === 'missing' && /not available/.test(err.message), 'open() error: ' + err.message);
same(err.missing, ['detector', 'classifier', 'species'], 'the error carries the missing list');
// No list given and no species folder in bro.models: the list is missing too
// (it was left out, so available() answered true without one).
// With no detector given and none found, `fetch` names what bro.models.fetch would install.
if (bro.models.find('species') === null) {
    same(wl.locate({ detector: empty, classifier: empty }).missing, ['detector', 'classifier', 'species'],
         'locate() with no species folder names the list');
    same(wl.locate({ detector: empty, classifier: empty }).fetch, ['species'],
         'no species folder: fetching it brings every list');
} else {
    console.log('(this machine has a species folder in bro.models: the no-folder case is not checked)');
}
if (bro.models.find('species') !== null && bro.models.find('species/upper-midwest-herps') === null) {
    const loc = wl.locate({ detector: empty, classifier: empty });
    same([loc.missing.includes('species'), loc.fetch], [true, ['species/upper-midwest-herps']],
         'a species folder without the herps: the herps folder alone is fetched');
} else if (bro.models.find('species/upper-midwest-herps') !== null) {
    const loc = wl.locate({ detector: empty, classifier: empty });
    assert(loc.species.length === 2 && /upper-midwest-herps\/species\.json$/.test(loc.species[1]), 'the herps list is the second default');
    assert(!loc.fetch.length, 'nothing of the lists to fetch: ' + loc.fetch.join());
}
same(wl.locate({ detector: empty, classifier: empty, species: path.join(empty, 'list.json') }).fetch, [],
     'paths given are never fetched');

// config() reads files, never weights: a list of two rows is counted.
const list = path.join(root, 'two.json');
fs.writeFileSync(list, JSON.stringify({ species: [
    { common: 'Mourning Dove', scientific: 'Zenaida macroura', class: 'Aves', order: 'Columbiformes', family: 'Columbidae' },
    { common: 'Eastern Chipmunk', scientific: 'Tamias striatus', class: 'Mammalia', order: 'Rodentia', family: 'Sciuridae' },
] }));
const cfg = wl.config({ detector: empty, classifier: empty, species: list, model: 'bioclip', threshold: 0.2 });
same(Object.keys(cfg), ['detector', 'classifier', 'threshold', 'queries', 'topK', 'margin', 'speciesList'], 'config keys, in order');
same([cfg.detector.repo, cfg.classifier.repo, cfg.classifier.weightsBytes], ['google/owlv2-base-patch16-ensemble', 'imageomics/bioclip', null], 'model identity');
same([cfg.threshold, cfg.topK, cfg.margin, cfg.speciesList.files, cfg.speciesList.count], [0.2, 5, 0.15, ['two.json'], 2], 'config values');
assert(/^[0-9a-f]{8}$/.test(cfg.speciesList.hash), 'the list hash: ' + cfg.speciesList.hash);
same(wl.readSpeciesList(list).map((s) => s.common), ['Mourning Dove', 'Eastern Chipmunk'], 'readSpeciesList');
assert(wl.speciesPrompt(wl.readSpeciesList(list)[1]) ===
       'a photo of Animalia Chordata Mammalia Rodentia Sciuridae Tamias striatus with common name Eastern Chipmunk.',
       'BioCLIP prompt: taxonomy (kingdom and phylum defaulted), then the common name');

// Several lists merge in order; a scientific name seen before (any case or
// spacing) is left out, so the first list's row wins.
const herpsDir = path.join(root, 'herps');
fs.mkdirSync(herpsDir, { recursive: true });
const list2 = path.join(herpsDir, 'species.json');
fs.writeFileSync(list2, JSON.stringify({ species: [
    { common: 'Common Garter Snake', scientific: 'Thamnophis sirtalis', class: 'Squamata', family: 'Colubridae' },
    { common: 'Dove (again)', scientific: 'zenaida  Macroura', class: 'Aves', order: 'Columbiformes', family: 'Columbidae' },
] }));
const both = wl.config({ detector: empty, classifier: empty, species: [list, list2] });
same([both.speciesList.files, both.speciesList.count], [['two.json', 'herps/species.json'], 3],
     'two lists: named (a folder\'s species.json by its folder) and merged');
assert(both.speciesList.hash !== cfg.speciesList.hash, 'the hash covers every list');
same(wl.readSpecies([list, list2]).map((s) => s.common), ['Mourning Dove', 'Eastern Chipmunk', 'Common Garter Snake'],
     'readSpecies: in order, the duplicate left out');
same(wl.readSpecies([list2, { common: 'Painted Turtle', scientific: 'Chrysemys picta', class: 'Testudines' }, list])
         .map((s) => s.common), ['Common Garter Snake', 'Dove (again)', 'Painted Turtle', 'Eastern Chipmunk'],
     'files and rows mixed; the first of a name wins');
same(wl.config({ detector: empty, classifier: empty, species: [list2, list] }).speciesList.count, 3, 'either order');
const editedDir = path.join(root, 'edited');
fs.mkdirSync(editedDir, { recursive: true });
const listEdited = path.join(editedDir, 'two.json');
fs.writeFileSync(listEdited, JSON.stringify([{ common: 'Mourning Dove', scientific: 'Zenaida macroura', class: 'Aves' },
                                             { common: 'Chipmunk', scientific: 'Tamias striatus', class: 'Mammalia' }]));
const edited = wl.config({ detector: empty, classifier: empty, species: listEdited, model: 'bioclip', threshold: 0.2 });
same([edited.speciesList.files, edited.speciesList.count], [['two.json'], 2], 'same name and count');
assert(edited.speciesList.hash !== cfg.speciesList.hash, 'an edited list is another config');
same(wl.locate({ detector: empty, classifier: empty, species: [list, path.join(root, 'nope.json')] }).missing,
     ['detector', 'classifier', 'species'], 'one list of several not there is missing');
assert(!wl.locate({ detector: empty, classifier: empty, species: [list, list2] }).missing.includes('species'), 'both there');
err = expectThrows(() => wl.open({ detector: empty, classifier: empty, species: [list, path.join(root, 'nope.json')] }), 'open() with a list not there');
if (bro.vision.loadOwlv2 && bro.lm && bro.lm.loadClip) assert(/species \(.*two\.json \+ .*nope\.json\)/.test(err.message), 'open() names the lists: ' + err.message);

// ── labels: 0.85 or more, a burst's consensus share else the photo's top-1 ──
const photoRec = (score, o) => Object.assign({ path: 'p', animal: true,
    species: [{ common: 'Robin', scientific: 'Turdus migratorius', score }, { common: 'Jay', scientific: 'Cyanocitta cristata', score: 0.01 }] }, o || {});
const burstOf = (share, o) => Object.assign({ count: 3, animalCount: 3,
    species: { common: 'Garter', scientific: 'Thamnophis sirtalis', share, agree: 2 } }, o || {});
same(wl.label(photoRec(0.9)), { common: 'Robin', scientific: 'Turdus migratorius', score: 0.9, from: 'photo' }, 'a photo alone: its top-1');
assert(wl.label(photoRec(0.85)) !== null, 'at the threshold: labelled');
assert(wl.label(photoRec(0.8499)) === null, 'under it: not identified');
same(wl.label(photoRec(0.99), burstOf(0.86)), { common: 'Garter', scientific: 'Thamnophis sirtalis', score: 0.86, from: 'burst' },
     'in a burst: the consensus, whatever the frame says');
assert(wl.label(photoRec(0.99), burstOf(0.6)) === null, 'a burst under the threshold: not identified, even with a sure frame');
same(wl.label(null, burstOf(0.9)).from, 'burst', 'a burst alone');
assert(wl.label(photoRec(0.5), burstOf(0.6), { labelThreshold: 0.55 }).common === 'Garter', 'labelThreshold');
assert(wl.label(photoRec(0.9), { count: 1, species: null }) !== null, 'a burst without a consensus: the photo\'s own');
assert(wl.label(photoRec(0.9, { animal: false })) === null && wl.label({ path: 'e', error: 'x' }) === null && wl.label(null) === null,
       'no animal, an error, nothing: null');

// ── pixels ───────────────────────────────────────────────────────────────
const sq = wl.subjectSquare([120, 40, 40, 20], 200, 100, { margin: 0.25 });
same([sq.w, sq.h], [60, 60], 'square of 40 * 1.5');
assert(sq.x <= 120 && sq.x + sq.w >= 160 && sq.y <= 40 && sq.y + sq.h >= 60, 'covers the box');
assert(wl.subjectSquare([120, 40, 40, 20], 200, 100).w === 52, 'default margin 15%');
assert(wl.subjectSquare([10, 10, 0, 5], 100, 100) === null, 'an empty box is no subject');
// Grown past the short side: each side is clamped to the frame on its own.
same(wl.subjectSquare([0, 0, 60, 70], 100, 80), { x: 0, y: 0, w: 91, h: 80 }, 'clamped to the frame');

const rgb = new Uint8Array(4 * 2 * 3);
for (let i = 0; i < 8; i++) rgb[i * 3] = i * 10;
const crop = wl.cropImage({ width: 4, height: 2, channels: 3, pixels: rgb }, { x: 1, y: 1, w: 2, h: 1 });
same([crop.width, crop.height, Array.from(crop.data)], [2, 1, [50, 0, 0, 255, 60, 0, 0, 255]], 'RGB crop to RGBA');

const W = 300, H = 150, thirds = new Uint8Array(W * H * 3);
for (let i = 0; i < W * H; i++) { const x = i % W; thirds[i * 3 + (x < 100 ? 0 : x < 200 ? 1 : 2)] = 255; }
const input = wl.squareInput({ width: W, height: H, channels: 3, pixels: thirds }, 224);
const mid = (112 * 224 + 112) * 4;
same([input.width, input.height, Array.from(input.data.slice(mid, mid + 4))], [224, 224, [0, 255, 0, 255]],
     'square input: the centre square of the frame');

// Sharpness: a box in source pixels is the rect at the image's scale.
const img = { width: 64, height: 48, channels: 3, pixels: new Uint8Array(64 * 48 * 3) };
for (let i = 0; i < img.pixels.length; i++) img.pixels[i] = (i * 7919) % 251;
const whole = wl.sharpness(img);
assert(whole.pixels > 0 && whole.tenengrad > 0 && whole.tenengrad === Math.round(whole.tenengrad * 100) / 100,
       'whole-frame sharpness, rounded to 2 places');
same(wl.sharpness(img, { box: [100, 50, 400, 300], source: { width: 640, height: 480 } }),
     wl.sharpness(img, { rect: { x: 10, y: 5, w: 40, h: 30 } }), 'source box -> image rect');

// ── bursts ───────────────────────────────────────────────────────────────
assert(wl.parseExifTime('2026-10-05T11:34:38.56-05:00') === Date.UTC(2026, 9, 5, 16, 34, 38, 560), 'EXIF time with offset');
assert(wl.parseExifTime('2026-10-05T11:34:38.5') === Date.UTC(2026, 9, 5, 11, 34, 38, 500), 'EXIF time without offset');
assert(wl.parseExifTime('not a date') === null, 'junk is null');

const rec = (p, t, o) => Object.assign({ path: p, time: t, animal: false, species: [],
    sharpness: { frame: { tenengrad: 1 }, subject: null } }, o || {});
const sp = (common, score) => ({ common, scientific: common + ' sp.', score });
const records = [
    rec('a', 0, { animal: true, species: [sp('Robin', 0.9), sp('Jay', 0.1)], sharpness: { frame: { tenengrad: 9 }, subject: { tenengrad: 5 } } }),
    rec('b', 300, { animal: true, species: [sp('Jay', 0.6), sp('Robin', 0.4)], sharpness: { frame: { tenengrad: 1 }, subject: { tenengrad: 7 } } }),
    rec('c', 600, { animal: true, species: [sp('Robin', 0.8)], sharpness: { frame: { tenengrad: 1 }, subject: { tenengrad: 6 } } }),
    rec('d', 5000, { sharpness: { frame: { tenengrad: 2 }, subject: null } }),
    rec('e', 5200, { error: 'decode failed' }),
    rec('f', 5400, { sharpness: { frame: { tenengrad: 3 }, subject: null } }),
];
const bursts = wl.groupBursts(records, { gapS: 1 });
same(bursts.map((b) => b.photos), [['a', 'b', 'c'], ['d', 'f']], 'bursts; the error is left out');
same(bursts[0].species, { common: 'Robin', scientific: 'Robin sp.', share: 0.7, agree: 2 }, 'score-weighted consensus');
same([bursts[0].keeper, bursts[0].keeperSharpness], ['b', 7], 'keeper: the sharpest subject, not the sharpest frame');
same([bursts[1].species, bursts[1].keeper, bursts[1].durationS, bursts[1].maxGapS], [null, 'f', 0.4, 0.4],
     'no animal: no species, the sharpest frame');
assert(wl.groupBursts(records, { gapS: 0.25 }).length === 5, 'a tighter gap splits');
const split = wl.groupBursts(records, { sameScene: (p, c) => ({ ncc: c.path === 'c' ? 0.2 : 0.9, split: c.path === 'c' }) });
same(split.map((b) => [b.count, b.startedBy, b.minNcc]), [[2, 'gap', 0.9], [1, 'scene', null], [2, 'gap', 0.9]], 'scene split');

// ── the same module in a Worker ──────────────────────────────────────────
let reply = null;
const w = new Worker('../vision/worker_wildlife.js');
w.onmessage = (e) => { reply = e.data; };
w.postMessage({ records, empty, list, list2 });
const deadline = Date.now() + 20000;
while (reply === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
assert(reply !== null, 'the worker replied');
assert(reply.ok, 'worker: ' + reply.error);
assert(reply.surface, 'bro.vision.wildlife in the worker');
assert(/main-realm only; in a Worker pass detector, classifier and species/.test(reply.noModels),
       'a worker without paths is told to pass them: ' + reply.noModels);
same(reply.missing, ['detector', 'classifier', 'species'], 'worker locate() with paths');
assert(reply.available === false, 'worker available() is false on an empty directory');
same(reply.bursts, bursts, 'the worker groups the same bursts');
same(reply.count, 2, 'worker config() counts the list');
same(reply.merged, 3, 'worker config() merges two lists');
same(reply.labels, [bursts[0].species.common, null], 'worker label(): the consensus at 0.7 with a 0.65 threshold, not at 0.85');
w.terminate();

try { fs.rmSync(root, { recursive: true, force: true }); } catch (_) {}
console.log('bro.vision.wildlife binding OK (weights-free)');
}

if (why) skipTest(why);
else run();
