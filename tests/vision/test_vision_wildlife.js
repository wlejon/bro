// bro.vision.wildlife without weights (docs/vision-api.js, "Wildlife"): the
// surface in the main realm and in a Worker, available() / locate() / open()
// on an empty model directory, config() without loading, the pixel helpers,
// and burst grouping on synthetic records (gap, consensus, keeper, scene
// split). tests/vision/test_vision_wildlife_models.js runs the models.

const fs = require('fs');
const os = require('os');
const path = require('path');

if (!bro.vision || bro.vision.available === false) skipTest('bro.vision is the stub');
const wl = bro.vision.wildlife;

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}
const same = (a, b, what) => assert(JSON.stringify(a) === JSON.stringify(b),
    what + ': ' + JSON.stringify(a) + ' vs ' + JSON.stringify(b));

// ── surface ──────────────────────────────────────────────────────────────
assert(typeof wl === 'object' && wl !== null, 'bro.vision.wildlife exists');
for (const f of ['open', 'available', 'locate', 'config', 'subjectSquare', 'cropImage', 'squareInput',
                 'sharpness', 'speciesPrompt', 'readSpeciesList', 'parseExifTime', 'keeperScore', 'groupBursts']) {
    assert(typeof wl[f] === 'function', 'wildlife.' + f + ' is a function');
}
assert(wl.QUERIES.length === 13 && wl.QUERIES[0] === 'a bird', 'the animal queries');
same([wl.DEFAULTS.threshold, wl.DEFAULTS.topK, wl.DEFAULTS.margin, wl.DEFAULTS.gapS, wl.DEFAULTS.model],
     [0.15, 5, 0.15, 1, 'bioclip-2'], 'defaults');

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

// config() reads files, never weights: a list of two rows is counted.
const list = path.join(root, 'two.json');
fs.writeFileSync(list, JSON.stringify({ species: [
    { common: 'Mourning Dove', scientific: 'Zenaida macroura', class: 'Aves', order: 'Columbiformes', family: 'Columbidae' },
    { common: 'Eastern Chipmunk', scientific: 'Tamias striatus', class: 'Mammalia', order: 'Rodentia', family: 'Sciuridae' },
] }));
const cfg = wl.config({ detector: empty, classifier: empty, species: list, model: 'bioclip', threshold: 0.2 });
same(Object.keys(cfg), ['detector', 'classifier', 'threshold', 'queries', 'topK', 'margin', 'speciesList'], 'config keys, in order');
same([cfg.detector.repo, cfg.classifier.repo, cfg.classifier.weightsBytes], ['google/owlv2-base-patch16-ensemble', 'imageomics/bioclip', null], 'model identity');
same([cfg.threshold, cfg.topK, cfg.margin, cfg.speciesList], [0.2, 5, 0.15, { file: 'two.json', count: 2 }], 'config values');
same(wl.readSpeciesList(list).map((s) => s.common), ['Mourning Dove', 'Eastern Chipmunk'], 'readSpeciesList');
assert(wl.speciesPrompt(wl.readSpeciesList(list)[1]) ===
       'a photo of Animalia Chordata Mammalia Rodentia Sciuridae Tamias striatus with common name Eastern Chipmunk.',
       'BioCLIP prompt: taxonomy (kingdom and phylum defaulted), then the common name');

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
w.postMessage({ records, empty, list });
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
w.terminate();

try { fs.rmSync(root, { recursive: true, force: true }); } catch (_) {}
console.log('bro.vision.wildlife binding OK (weights-free)');
