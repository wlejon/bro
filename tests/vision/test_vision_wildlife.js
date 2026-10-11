// bro.vision.wildlife without weights (docs/vision-api.js, "Wildlife"): the
// surface in the main realm and in a Worker, available() / locate() / open()
// on an empty model directory, config() without loading, the pixel helpers,
// and burst grouping on synthetic records (gap, consensus, keeper, scene
// split); ranks (taxaScores, classifyCrop with 'all' and 'byQuery'), labels
// at the narrowest sure rank, several animals in a photo, per-taxon burst
// labels, old records and bursts, and taxa maps merged across lists.
// tests/vision/test_vision_wildlife_models.js runs the models.

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
                 'sharpness', 'speciesPrompt', 'readSpeciesList', 'readSpecies', 'readTaxa', 'label', 'labels',
                 'formatLabel', 'taxaScores', 'classifyCrop', 'queryClasses', 'lineage', 'parseExifTime',
                 'keeperScore', 'groupBursts']) {
    assert(typeof wl[f] === 'function', 'wildlife.' + f + ' is a function');
}
assert(wl.QUERIES.length === 28 && wl.QUERIES[0] === 'a bird' && wl.QUERIES[12] === 'a mink', 'the animal queries');
same(wl.QUERIES.slice(13, 19), ['a snake', 'a turtle', 'a frog', 'a toad', 'a lizard', 'a salamander'],
     'then the reptiles and amphibians');
same(wl.QUERIES.slice(19), ['a dog', 'a cat', 'an insect', 'a dragonfly', 'a butterfly', 'a moth', 'a bee', 'a beetle', 'a spider'],
     'then pets and arthropods');
same([wl.DEFAULTS.threshold, wl.DEFAULTS.topK, wl.DEFAULTS.margin, wl.DEFAULTS.gapS, wl.DEFAULTS.model, wl.DEFAULTS.labelThreshold,
      wl.DEFAULTS.maxClassify, wl.DEFAULTS.candidates],
     [0.15, 5, 0.15, 1, 'bioclip-2', 0.85, 8, 'all'], 'defaults');
same([wl.DEFAULTS.minBoxScore, wl.DEFAULTS.broadMinBoxScore, wl.DEFAULTS.burstMinBoxScore, wl.DEFAULTS.queryMatch,
      wl.DEFAULTS.consensus], [0.3, 0.3, 0.4, true, true], 'the box rule defaults');
same(wl.SPECIES_LISTS, [{ folder: 'species', file: 'upper-midwest.json' },
                        { folder: 'species/upper-midwest-herps', file: 'species.json' },
                        { folder: 'species/upper-midwest-arthropods', file: 'species.json' },
                        { folder: 'species/pets', file: 'species.json' }], 'the default species lists');
same(wl.RANKS, ['species', 'genus', 'family', 'order', 'class'], 'the ranks, narrowest first');
// Every query has the classes its crop ranks over with candidates 'byQuery'.
for (const q of wl.QUERIES) assert(Array.isArray(wl.queryClasses(q)), 'classes for ' + q);
same([wl.queryClasses('a bird'), wl.queryClasses('a dog'), wl.queryClasses('a moth'), wl.queryClasses('a spider'),
      wl.queryClasses('a toad'), wl.queryClasses('a rock')],
     [['Aves'], ['Mammalia'], ['Insecta'], ['Arachnida'], ['Amphibia'], null], 'query classes');
assert(wl.queryClasses('a turtle').includes('Testudines') && wl.queryClasses('a snake').includes('Squamata'), 'reptile classes');

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
if (bro.models.find('species') !== null) {
    // A species folder: each list folder of its own that is not there is fetched alone.
    const loc = wl.locate({ detector: empty, classifier: empty });
    const absent = wl.SPECIES_LISTS.filter((l) => l.folder !== 'species' && bro.models.find(l.folder) === null)
        .map((l) => l.folder);
    same(loc.fetch, absent, 'the list folders not there, each alone');
    assert(loc.missing.includes('species') === absent.length > 0, 'the lists missing exactly when one is not there');
    assert(loc.species.length === 4 && loc.species.every((s, i) => (s === null) === absent.includes(wl.SPECIES_LISTS[i].folder)),
           'a file for each list found, null for each not');
    if (bro.models.find('species/upper-midwest-herps') !== null) {
        assert(/upper-midwest-herps\/species\.json$/.test(loc.species[1]), 'the herps list is the second default');
    }
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
same(Object.keys(cfg), ['detector', 'classifier', 'threshold', 'queries', 'topK', 'margin', 'maxClassify', 'candidates',
                        'speciesList'], 'config keys, in order');
same([cfg.detector.repo, cfg.classifier.repo, cfg.classifier.weightsBytes], ['google/owlv2-base-patch16-ensemble', 'imageomics/bioclip', null], 'model identity');
same([cfg.threshold, cfg.topK, cfg.margin, cfg.maxClassify, cfg.candidates, cfg.speciesList.files, cfg.speciesList.count],
     [0.2, 5, 0.15, 8, { mode: 'all' }, ['two.json'], 2], 'config values');
same(wl.config({ detector: empty, classifier: empty, species: list, candidates: 'byQuery', maxClassify: 3 }).candidates,
     { mode: 'byQuery', fallback: 0.85 }, 'byQuery is in the config, with its fallback threshold');
same(wl.config({ detector: empty, classifier: empty, species: list, candidates: 'byQuery', labelThreshold: 0.9 }).candidates,
     { mode: 'byQuery', fallback: 0.9 }, 'the fallback is labelThreshold');
expectThrows(() => wl.config({ detector: empty, classifier: empty, species: list, candidates: 'some' }), 'an unknown candidates mode');
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
// These are records and bursts from before `animals` and burst `taxa`.
same(wl.label(photoRec(0.9)), { common: 'Robin', scientific: 'Turdus migratorius', rank: 'species', score: 0.9, from: 'photo', boxes: [] },
     'a photo alone: its top-1');
assert(wl.label(photoRec(0.85)) !== null, 'at the threshold: labelled');
assert(wl.label(photoRec(0.8499)) === null, 'under it: not identified');
same(wl.label(photoRec(0.99), burstOf(0.86)), { common: 'Garter', scientific: 'Thamnophis sirtalis', rank: 'species', score: 0.86,
                                                from: 'burst', boxes: [], frames: 2, share: 0.86 },
     'in a burst: the consensus, whatever the frame says');
assert(wl.label(photoRec(0.8), burstOf(0.6)) === null, 'a burst under the threshold and a frame under it: not identified');
same(wl.label(photoRec(0.99), burstOf(0.6)).from, 'photo', 'a burst with no label: a sure frame its own');
same(wl.label(null, burstOf(0.9)).from, 'burst', 'a burst alone');
assert(wl.label(photoRec(0.5), burstOf(0.6), { labelThreshold: 0.55 }).common === 'Garter', 'labelThreshold');
assert(wl.label(photoRec(0.9), { count: 1, species: null }) !== null, 'a burst without a consensus: the photo\'s own');
assert(wl.label(photoRec(0.9, { animal: false })) === null && wl.label({ path: 'e', error: 'x' }) === null && wl.label(null) === null,
       'no animal, an error, nothing: null');
// An old record: its top K is the one crop, on the best box, its genus and up summed over the top K.
const oldRec = { path: 'o', animal: true, detections: [{ label: 'a bird', score: 0.5, box: [1, 2, 3, 4] }],
    species: [{ common: 'Robin', scientific: 'Turdus migratorius', family: 'Turdidae', score: 0.5 },
              { common: 'Varied Thrush', scientific: 'Ixoreus naevius', family: 'Turdidae', score: 0.4 }] };
same(wl.labels(oldRec), [{ common: 'Turdidae', scientific: 'Turdidae', rank: 'family', score: 0.9, from: 'photo', boxes: [[1, 2, 3, 4]] }],
     'an old record falls back a rank over its top K');

// ── ranks: taxon scores, a crop's ranking, labels at the narrowest sure rank ──
const near = (a, b, what) => assert(Math.abs(a - b) < 1e-4, what + ': ' + a + ' vs ' + b);
const ROWS = [
    { common: 'Mallard', scientific: 'Anas platyrhynchos', class: 'Aves', order: 'Anseriformes', family: 'Anatidae' },
    { common: 'American Black Duck', scientific: 'Anas rubripes', class: 'Aves', order: 'Anseriformes', family: 'Anatidae' },
    { common: 'Wood Duck', scientific: 'Aix sponsa', class: 'Aves', order: 'Anseriformes', family: 'Anatidae' },
    { common: 'Great Egret', scientific: 'Ardea alba', class: 'Aves', order: 'Pelecaniformes', family: 'Ardeidae' },
    { common: 'Great Blue Heron', scientific: 'Ardea herodias', class: 'Aves', order: 'Pelecaniformes', family: 'Ardeidae' },
    { common: 'Common Green Darner', scientific: 'Anax junius', class: 'Insecta', order: 'Odonata', family: 'Aeshnidae' },
    { common: 'Canada Darner', scientific: 'Aeshna canadensis', class: 'Insecta', order: 'Odonata', family: 'Aeshnidae' },
    { common: 'Twelve-spotted Skimmer', scientific: 'Libellula pulchella', class: 'Insecta', order: 'Odonata', family: 'Libellulidae' },
    { common: 'Monarch', scientific: 'Danaus plexippus', class: 'Insecta', order: 'Lepidoptera', family: 'Nymphalidae' },
    { common: 'Eastern Chipmunk', scientific: 'Tamias striatus', class: 'Mammalia', order: 'Rodentia', family: 'Sciuridae' },
];
// Similarities whose softmax (at the model's 100) is `p`; the rows not named share what is left.
function simsFor(p) {
    const named = Object.keys(p);
    const other = Math.max(1e-9, 1 - named.reduce((s, k) => s + p[k], 0)) / (ROWS.length - named.length);
    return ROWS.map((r) => Math.log(p[r.common] != null ? p[r.common] : other) / 100);
}
same(wl.lineage(ROWS[0]), { genus: 'Anas', family: 'Anatidae', order: 'Anseriformes', class: 'Aves' }, 'a species lineage');
const ts = wl.taxaScores(ROWS, [0.5, 0.4, 0.1, 0, 0, 0, 0, 0, 0, 0]);
same(ts.genus, [{ scientific: 'Anas', score: 0.9, family: 'Anatidae', order: 'Anseriformes', class: 'Aves' },
                { scientific: 'Aix', score: 0.1, family: 'Anatidae', order: 'Anseriformes', class: 'Aves' }],
     'genus: the sum over its species, with its lineage');
same([ts.family, ts.order, ts.class].map((l) => l.map((t) => [t.scientific, t.score])),
     [[['Anatidae', 1]], [['Anseriformes', 1]], [['Aves', 1]]], 'family, order, class');
same(Object.keys(ts.family[0]), ['scientific', 'score', 'order', 'class'], 'a family knows its order and class');
assert(wl.taxaScores(ROWS, ROWS.map(() => 0.1)).genus.length === 3 && wl.taxaScores(ROWS, ROWS.map(() => 0.1), { top: 0 }).genus.length === 8,
       'top 3 a rank, or all');
same(wl.taxaScores([{ scientific: 'Foo bar', class: 'Aves' }], [1]),
     { genus: [{ scientific: 'Foo', score: 1, class: 'Aves' }], family: [], order: [], class: [{ scientific: 'Aves', score: 1 }] },
     'a row without a family or order adds nothing there');

const c1 = wl.classifyCrop(ROWS, simsFor({ Mallard: 0.9 }));
same([c1.species.length, c1.species[0].common, c1.candidates], [5, 'Mallard', 'all'], 'classifyCrop: top 5 over every row');
near(c1.species[0].score, 0.9, 'the softmax of log(p) / 100 is p');
same(Object.keys(c1.taxa), ['genus', 'family', 'order', 'class'], 'and the taxa');
// byQuery: the softmax over the query's classes.
const cq = wl.classifyCrop(ROWS, simsFor({ Mallard: 0.4, 'Common Green Darner': 0.5, 'American Black Duck': 0.1 }),
                           { candidates: 'byQuery', query: 'a bird' });
same([cq.candidates, cq.species[0].common, cq.species.every((s) => s.class === 'Aves')], ['query', 'Mallard', true], 'byQuery: birds only');
assert(cq.species[0].score > 0.79, 'renormalised over the birds: ' + cq.species[0].score);
same(wl.classifyCrop(ROWS, simsFor({ Mallard: 0.4, 'Common Green Darner': 0.5 }), { query: 'a bird' }).species[0].common,
     'Common Green Darner', "'all' ignores the query");
same(wl.classifyCrop(ROWS, simsFor({ Mallard: 0.9 }), { candidates: 'byQuery', query: 'a rock' }).candidates, 'all',
     'a query without classes ranks over every row');
// Sure of a bird while the detector said moth: the bird is the answer.
const cf = wl.classifyCrop(ROWS, simsFor({ Mallard: 0.9, 'Common Green Darner': 0.04, Monarch: 0.04 }),
                           { candidates: 'byQuery', query: 'a moth' });
same([cf.candidates, cf.species[0].common], ['fallback', 'Mallard'], 'byQuery falls back to a sure answer of another class');
const cn = wl.classifyCrop(ROWS, simsFor({ Mallard: 0.6, 'Common Green Darner': 0.2, Monarch: 0.2 }),
                           { candidates: 'byQuery', query: 'a moth' });
same([cn.candidates, cn.species[0].class], ['query', 'Insecta'], 'not sure of the other class: the query\'s');
same(wl.classifyCrop(ROWS, simsFor({ Mallard: 0.9, 'Common Green Darner': 0.04, Monarch: 0.04 }),
                     { candidates: 'byQuery', query: 'a moth', fallback: 0.95 }).candidates, 'query', 'fallback threshold');

let boxN = 0;
const animal = (p, o) => {
    const c = wl.classifyCrop(ROWS, simsFor(p), o);
    return { box: (o && o.box) || [10 * ++boxN, 10, 50, 40], score: 0.5, label: (o && o.query) || 'a bird',
             subject: null, species: c.species, taxa: c.taxa, candidates: c.candidates };
};
const photo = (p, t, crops) => ({ path: p, time: t, animal: crops.length > 0,
    detections: crops.map((c) => ({ label: c.label, score: c.score, box: c.box })), animals: crops, animalsDropped: 0,
    subject: null, species: crops.length ? crops[0].species : [], sharpness: { frame: { tenengrad: 1 }, subject: null } });
const TAXA = { 'Anas': { rank: 'genus', common: 'Dabbling Ducks' }, 'Aeshnidae': { rank: 'family', common: 'Darners' } };
const L = (rec, burst, t) => wl.labels(rec, burst, { taxa: TAXA, labelThreshold: t });
const brief = (ls) => ls.map((l) => [l.rank, l.scientific, l.boxes.length]);
const one = (p, t) => brief(L(photo('x', 0, [animal(p)]), null, t));

same(one({ Mallard: 0.9 }), [['species', 'Anas platyrhynchos', 1]], 'species clears');
const gl = L(photo('x', 0, [animal({ Mallard: 0.5, 'American Black Duck': 0.4 })]));
same(brief(gl), [['genus', 'Anas', 1]], 'else the genus');
same([gl[0].common, wl.formatLabel(gl[0]), gl[0].from], ['Dabbling Ducks', 'Dabbling Ducks (Anas)', 'photo'], 'a genus named by the taxa map');
near(gl[0].score, 0.9, 'a genus scores its species summed');
same(one({ Mallard: 0.3, 'American Black Duck': 0.15, 'Wood Duck': 0.45 }), [['family', 'Anatidae', 1]], 'else the family');
const dl = L(photo('x', 0, [animal({ 'Common Green Darner': 0.5, 'Canada Darner': 0.4 })]));
same([dl[0].rank, dl[0].common, dl[0].scientific, wl.formatLabel(dl[0])], ['family', 'Darners', 'Aeshnidae', 'Darners (Aeshnidae)'],
     'Darners (Aeshnidae)');
same(wl.formatLabel(L(photo('x', 0, [animal({ Mallard: 0.3, 'American Black Duck': 0.15, 'Wood Duck': 0.45 })]))[0]), 'Anatidae',
     'a family with no common name: its scientific name');
same(brief(L(photo('x', 0, [animal({ 'Common Green Darner': 0.45, 'Twelve-spotted Skimmer': 0.45 }, { query: 'a dragonfly' })]))),
     [['order', 'Odonata', 1]], 'else the order');
same(one({ Mallard: 0.3, 'Great Egret': 0.3, 'Great Blue Heron': 0.25 }), [['class', 'Aves', 1]], 'else the class');
same(one({ Mallard: 0.3, Monarch: 0.3, 'Eastern Chipmunk': 0.3 }), [], 'else nothing: found, not identified');
same(one({ Mallard: 0.5, 'American Black Duck': 0.4 }, 0.95), [], 'the threshold at every rank');
same(one({ Mallard: 0.5, 'American Black Duck': 0.4 }, 0.93), [['class', 'Aves', 1]], 'a lower threshold reaches the class');
same(one({ Mallard: 0.5, 'American Black Duck': 0.4 }, 0.5), [['species', 'Anas platyrhynchos', 1]], 'a low one the species');
same(wl.label(photo('x', 0, [animal({ Mallard: 0.5, 'American Black Duck': 0.4 })]), null, { taxa: TAXA }).rank, 'genus', 'label() has the rank');

// Several animals in one photo.
const two = L(photo('x', 0, [animal({ Mallard: 0.9 }, { box: [1, 1, 5, 5] }), animal({ Mallard: 0.92 }, { box: [9, 9, 5, 5] })]));
same(two.map((l) => [l.scientific, l.boxes]), [['Anas platyrhynchos', [[1, 1, 5, 5], [9, 9, 5, 5]]]], 'one species twice: one label, both boxes');
near(two[0].score, 0.92, 'its best score');
same(brief(L(photo('x', 0, [animal({ Mallard: 0.9 }), animal({ 'Great Egret': 0.95 })]))),
     [['species', 'Ardea alba', 1], ['species', 'Anas platyrhynchos', 1]], 'an egret and a mallard: two labels, best first');
same(brief(L(photo('x', 0, [animal({ Mallard: 0.9 }), animal({ Mallard: 0.5, 'American Black Duck': 0.4 })]))),
     [['species', 'Anas platyrhynchos', 2]], 'a genus with its one species inside: left out, its box joins the species');
same(brief(L(photo('x', 0, [animal({ Mallard: 0.9 }), animal({ 'American Black Duck': 0.9 }),
                            animal({ Mallard: 0.5, 'American Black Duck': 0.4 })]))),
     [['species', 'Anas platyrhynchos', 1], ['species', 'Anas rubripes', 1]], 'two species inside: the genus left out, its box with neither');
same(brief(L(photo('x', 0, [animal({ 'Common Green Darner': 0.5, 'Canada Darner': 0.4 }), animal({ 'Common Green Darner': 0.9 })]))),
     [['species', 'Anax junius', 2]], 'a family with a species of it');
same(brief(L(photo('x', 0, [animal({ Mallard: 0.9 }), animal({ 'Common Green Darner': 0.5, 'Canada Darner': 0.4 }),
                            animal({ Mallard: 0.3, Monarch: 0.3, 'Eastern Chipmunk': 0.3 })]))),
     [['species', 'Anas platyrhynchos', 1], ['family', 'Aeshnidae', 1]], 'a duck, a darner, and an unidentified crop');

// Bursts: a label for every taxon, shown on every frame with that frame's boxes.
const bRecs = [
    photo('b1', 0, [animal({ 'Great Egret': 0.95 }, { box: [1, 0, 2, 2] })]),
    photo('b2', 300, [animal({ 'Great Egret': 0.92 }, { box: [2, 0, 2, 2] })]),
    photo('b3', 600, [animal({ Mallard: 0.9 }, { box: [3, 0, 2, 2] })]),
    photo('h1', 5000, [animal({ 'Great Blue Heron': 0.95 }, { box: [4, 0, 2, 2] })]),
    photo('h2', 5300, [animal({ 'Great Blue Heron': 0.9 }, { box: [5, 0, 2, 2] })]),
    photo('h3', 5600, [animal({ 'Great Blue Heron': 0.8, 'Great Egret': 0.15 }, { box: [6, 0, 2, 2] })]),
    photo('n1', 9000, []),
];
const bb = wl.groupBursts(bRecs, { gapS: 1 });
same(bb.map((b) => b.photos), [['b1', 'b2', 'b3'], ['h1', 'h2', 'h3'], ['n1']], 'three bursts');
assert(Array.isArray(bb[0].taxa) && bb[0].taxa.every((T) => wl.RANKS.includes(T.rank) && T.scores.length >= 1 && T.scores.length <= 3),
       'a burst keeps every taxon\'s frame scores');
same(bb[0].species.common, 'Great Egret', 'and the old consensus');
const egret = bb[0].taxa.find((T) => T.scientific === 'Ardea alba');
same([egret.rank, egret.common, egret.genus, egret.family, egret.scores.slice(0, 2)], ['species', 'Great Egret', 'Ardea', 'Ardeidae', [0.95, 0.92]],
     'a taxon: its frame scores, best first');
const l1 = L(bRecs[0], bb[0]), l3 = L(bRecs[2], bb[0]);
same(l1.map((l) => [l.scientific, l.from, l.frames, l.boxes]), [['Ardea alba', 'burst', 2, [[1, 0, 2, 2]]], ['Anas platyrhynchos', 'burst', 1, []]],
     'an egret in 2 of 3 frames and a mallard in 1: both, on a frame with the egret');
same(l3.map((l) => [l.scientific, l.boxes]), [['Ardea alba', []], ['Anas platyrhynchos', [[3, 0, 2, 2]]]], 'and on the mallard\'s frame');
near(l1[0].score, 0.95, 'a burst label scores its best frame');
near(l1[0].share, (0.95 + 0.92 + egret.scores[2]) / 3, 'its share is the mean over the animal frames');
same(brief(L(null, bb[0])), [['species', 'Ardea alba', 0], ['species', 'Anas platyrhynchos', 0]], 'labels(null, burst)');
// A heron sure in two frames and less in the third (alone there only its genus).
same(brief(L(bRecs[5])), [['genus', 'Ardea', 1]], 'the weak frame alone: its genus');
const lh = L(bRecs[5], bb[1]);
same(lh.map((l) => [l.rank, l.scientific, l.from, l.frames, l.boxes]), [['species', 'Ardea herodias', 'burst', 2, [[6, 0, 2, 2]]]],
     'in its burst: the heron, its box');
near(lh[0].share, 0.8833, 'the consensus over the three');
same(brief(L(bRecs[5], bb[1], 0.96)), [['class', 'Aves', 1]], 'the threshold is applied at label time');
same(L(bRecs[6], bb[2]), [], 'a burst with no animal: nothing');
same(bb[2].taxa, [], 'and no taxa');
// Old records in a burst: their top K.
const ob = wl.groupBursts([photoRec(0.9, { time: 0, path: 'o1' }), photoRec(0.8, { time: 100, path: 'o2' })]);
same(L(photoRec(0.8, { path: 'o2' }), ob[0]).map((l) => [l.scientific, l.frames, l.from]), [['Turdus migratorius', 1, 'burst']],
     'old records group per taxon too');

// ── the box rule: what weak detector boxes may name ──────────────────────
const boxed = (p, score, query) => Object.assign(animal(p, query ? { query } : undefined), { score });
const names = (ls) => ls.map((l) => l.scientific);
// A box other than the photo's best names nothing under minBoxScore (0.3).
const weak2 = photo('w', 0, [boxed({ 'Great Egret': 0.95 }, 0.5), boxed({ Mallard: 0.9 }, 0.2)]);
same(names(L(weak2)), ['Ardea alba'], 'a weak secondary box names nothing');
same(names(wl.labels(weak2, null, { minBoxScore: 0 })), ['Ardea alba', 'Anas platyrhynchos'], 'minBoxScore 0: it does');
same(names(L(photo('w', 0, [boxed({ 'Great Egret': 0.95 }, 0.5), boxed({ Mallard: 0.9 }, 0.3)]))), ['Ardea alba', 'Anas platyrhynchos'],
     'at minBoxScore it names');
same(names(L(photo('w', 0, [boxed({ Mallard: 0.9 }, 0.16)]))), ['Anas platyrhynchos'], 'the photo\'s best box names at any score');
// A family, order or class needs a box at broadMinBoxScore (0.3); a genus does not.
const darner = (s) => photo('d', 0, [boxed({ 'Common Green Darner': 0.5, 'Canada Darner': 0.4 }, s, 'a dragonfly')]);
same(brief(L(darner(0.2))), [], 'a family on a weak box: not named');
same(brief(L(darner(0.35))), [['family', 'Aeshnidae', 1]], 'on a sure one: named');
same(brief(wl.labels(darner(0.2), null, { broadMinBoxScore: 0 })), [['family', 'Aeshnidae', 1]], 'broadMinBoxScore 0: named');
same(brief(L(photo('g', 0, [boxed({ Mallard: 0.5, 'American Black Duck': 0.4 }, 0.2)]))), [['genus', 'Anas', 1]],
     'a genus on a weak box: named');
// queryMatch: an order or class outside the box's query's classes is not named; a species is.
const odo = (q) => photo('q', 0, [boxed({ 'Common Green Darner': 0.45, 'Twelve-spotted Skimmer': 0.45 }, 0.6, q)]);
same(brief(L(odo('a bird'))), [], 'a "bird" box: no insect order');
same(brief(L(odo('a dragonfly'))), [['order', 'Odonata', 1]], 'a "dragonfly" box: the order');
same(brief(wl.labels(odo('a bird'), null, { queryMatch: false })), [['order', 'Odonata', 1]], 'queryMatch false: the order');
same(brief(L(photo('q', 0, [boxed({ 'Common Green Darner': 0.9 }, 0.6, 'a bird')]))), [['species', 'Anax junius', 1]],
     'a species of another class on a "bird" box: named');
same(brief(L(photo('q', 0, [boxed({ Mallard: 0.3, 'Great Egret': 0.3, 'Great Blue Heron': 0.25 }, 0.6, 'a rock')]))), [['class', 'Aves', 1]],
     'a query without classes matches any');
// Bursts: a name one frame reaches needs that frame's box at burstMinBoxScore (0.4).
const egretBurst = (mallardBox) => {
    const recs = [0, 1, 2].map((i) => photo('e' + i, i * 300, [boxed({ 'Great Egret': 0.95 }, 0.6, null)]
        .concat(i === 1 ? [boxed({ Mallard: 0.9 }, mallardBox)] : [])));
    return { recs, burst: wl.groupBursts(recs, { gapS: 1 })[0] };
};
const wb = egretBurst(0.35);
assert(wb.burst.taxa.every((T) => Array.isArray(T.evidence) && T.evidence.length === T.scores.length),
       'a burst taxon keeps its evidence, one entry a frame');
same(wb.burst.taxa.find((T) => T.scientific === 'Anas platyrhynchos').evidence[1], [0.0056, 0.6, 3, 0.9, 0.35, 2],
     'evidence: [score, box score, flags] a crop (1: the best box, 2: its query matches)');
same(names(L(null, wb.burst)), ['Ardea alba'], 'one frame\'s mallard on a weak box: not a burst name');
same(wb.recs.map((r) => names(L(r, wb.burst))), [['Ardea alba'], ['Ardea alba'], ['Ardea alba']], 'on no frame');
same(names(wl.labels(null, wb.burst, { burstMinBoxScore: 0 })), ['Ardea alba', 'Anas platyrhynchos'], 'burstMinBoxScore 0: it is');
const sb = egretBurst(0.6);
same(sb.recs.map((r) => names(L(r, sb.burst))), [['Ardea alba', 'Anas platyrhynchos'], ['Ardea alba', 'Anas platyrhynchos'],
     ['Ardea alba', 'Anas platyrhynchos']], 'on a sure box: a burst name on every frame');
same(L(sb.recs[1], sb.burst).find((l) => l.scientific === 'Anas platyrhynchos').boxes, [sb.recs[1].animals[1].box],
     'with its frame\'s box');
// Old bursts (no evidence) are weighed by their scores alone.
const oldWb = Object.assign({}, wb.burst, { taxa: wb.burst.taxa.map((T) => { const c = Object.assign({}, T); delete c.evidence; return c; }) });
same(names(L(null, oldWb)), ['Ardea alba', 'Anas platyrhynchos'], 'a burst grouped before the rule: its scores');
// Consensus: two frames or more agreeing on boxes under minBoxScore, at the threshold.
const consensus = (frames) => {
    const recs = [0, 1, 2].map((i) => photo('c' + i, i * 300, [boxed({ 'Great Egret': 0.95 }, 0.6)]
        .concat(i < frames ? [boxed({ Mallard: 0.95 }, 0.2)] : [])));
    return { recs, burst: wl.groupBursts(recs, { gapS: 1 })[0] };
};
const cb = consensus(3);
same(names(L(null, cb.burst)).sort(), ['Anas platyrhynchos', 'Ardea alba'], 'three frames agreeing on a weak box: a burst name');
same(L(cb.recs[0], cb.burst).find((l) => l.scientific === 'Anas platyrhynchos').boxes, [cb.recs[0].animals[1].box], 'with its weak box');
same(names(wl.labels(null, cb.burst, { consensus: false })), ['Ardea alba'], 'consensus false: not');
same(names(L(null, consensus(1).burst)), ['Ardea alba'], 'one frame on a weak box: not');

// ── taxa maps, merged across lists ───────────────────────────────────────
const taxaA = path.join(root, 'taxa-a.json'), taxaB = path.join(root, 'taxa-b.json'), taxaA2 = path.join(root, 'taxa-a2.json');
const rowsA = ROWS.slice(0, 3), rowsB = ROWS.slice(5, 8);
fs.writeFileSync(taxaA, JSON.stringify({ species: rowsA, taxa: { Anas: { rank: 'genus', common: 'Dabbling Ducks' } } }));
fs.writeFileSync(taxaA2, JSON.stringify({ species: rowsA, taxa: { Anas: { rank: 'genus', common: 'Puddle Ducks' } } }));
fs.writeFileSync(taxaB, JSON.stringify({ species: rowsB, taxa: { ' anas ': { rank: 'genus', common: 'Other' },
    Aeshnidae: { rank: 'family', common: 'Darners' }, Odonata: { rank: 'order' } } }));
same(wl.readTaxa([taxaA, taxaB]), { Anas: { rank: 'genus', common: 'Dabbling Ducks' }, Aeshnidae: { rank: 'family', common: 'Darners' },
                                    Odonata: { rank: 'order' } }, 'readTaxa: merged, the first of a name wins');
same(wl.readTaxa([list, { species: [], taxa: { Aix: { rank: 'genus' } } }]), { Aix: { rank: 'genus' } },
     'a list without taxa adds none; a document item adds its own');
same(wl.readSpecies([taxaA, { species: [ROWS[9]], taxa: {} }]).map((r) => r.common), ['Mallard', 'American Black Duck', 'Wood Duck', 'Eastern Chipmunk'],
     'readSpecies takes documents too');
same(wl.config({ detector: empty, classifier: empty, species: [taxaA, taxaB] }).speciesList.hash,
     wl.config({ detector: empty, classifier: empty, species: [taxaA2, taxaB] }).speciesList.hash,
     'a taxa name is not in the hash: records stand');
same(wl.labels(photo('x', 0, [animal({ Mallard: 0.5, 'American Black Duck': 0.4 })]), null,
               { taxa: wl.readTaxa([taxaA2, taxaB]) })[0].common, 'Puddle Ducks', 'names come from the map at label time');

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
const cfSims = simsFor({ Mallard: 0.9, 'Common Green Darner': 0.04, Monarch: 0.04 });
w.postMessage({ records, empty, list, list2, bRecs, rows: ROWS, cfSims, taxa: TAXA });
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
same(reply.labels, ['Robin', 'Robin', null],
     'worker label(): Robin at 0.65 (share 0.7), at 0.85 (sure in one frame), not at 0.95');
same(reply.bBursts, bb, 'the worker groups bursts with taxa the same');
same(reply.bLabels, bRecs.map((r) => L(r, bb.find((b) => b.photos.includes(r.path)))), 'worker labels() of every frame');
same(reply.bAlone, bRecs.map((r) => L(r)), 'worker labels() of every photo alone');
same(reply.crop, wl.classifyCrop(ROWS, cfSims, { candidates: 'byQuery', query: 'a moth' }), 'worker classifyCrop');
w.terminate();

try { fs.rmSync(root, { recursive: true, force: true }); } catch (_) {}
console.log('bro.vision.wildlife binding OK (weights-free)');
}

if (why) skipTest(why);
else run();
