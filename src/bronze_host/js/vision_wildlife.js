// bro.vision.wildlife — what is in a wildlife photo, and which shot of a burst
// to keep (docs/vision-api.js, "Wildlife"). Pure JS over bro.vision.loadOwlv2,
// bro.lm.loadClip and bro.image: OWLv2 finds the animal from text queries, the
// boxes padded to squares are BioCLIP's input, and BioCLIP names each
// animal's species zero-shot over a species list (one prompt per species,
// embedded once), a genus and up by summing its species. labels() names
// each animal at the narrowest rank it is sure of, per photo or per burst.
// Sharpness and burst grouping pick the keeper of a run of shots.
//
// HOW IT IS SHIPPED. Compiled by bronze at build time (bro_compile_js in
// ../CMakeLists.txt, against js/module.globals) and entered from
// installVisionWildlifeModule() (host_js_modules.cpp) in the main realm
// (dom_globals.cpp) and in every Worker realm (installWorkerBroRoot), after
// the sibling APIs built `bro.vision`. The module assigns its surface onto
// `globalThis.__bro_vision_wildlife` and the installer mounts that object as
// `bro.vision.wildlife`. It reads nothing at load: bro.vision, bro.lm,
// bro.image, bro.models and require() are read off globalThis at the call,
// so a Worker (no bro.models) gets the same module and passes paths.
//
// broworkshop's ai/birds batch runs on this module; its record format is
// RECORDS.md there, so a change to a record (the queries, threshold, NMS,
// square margin, prompt recipe, temperature, ranking, rounding, fields) goes
// together with that file.
(function () {
    'use strict';

    const wl = {};

    /** OWLv2 queries: the animals of a yard species list, its reptiles and amphibians, pets, arthropods. */
    const QUERIES = [
        'a bird', 'a squirrel', 'a chipmunk', 'a rabbit', 'a deer', 'a raccoon', 'a fox',
        'a woodchuck', 'a skunk', 'a beaver', 'a muskrat', 'an otter', 'a mink',
        'a snake', 'a turtle', 'a frog', 'a toad', 'a lizard', 'a salamander',
        'a dog', 'a cat',
        'an insect', 'a dragonfly', 'a butterfly', 'a moth', 'a bee', 'a beetle', 'a spider',
    ];
    // The classes a query's crop is ranked over when candidates is 'byQuery'
    // (a query not here ranks over every species).
    const QUERY_CLASSES = {};
    const setClasses = (qs, classes) => qs.forEach((q) => { QUERY_CLASSES[q] = classes; });
    setClasses(['a bird'], ['Aves']);
    setClasses(['a squirrel', 'a chipmunk', 'a rabbit', 'a deer', 'a raccoon', 'a fox', 'a woodchuck',
                'a skunk', 'a beaver', 'a muskrat', 'an otter', 'a mink', 'a dog', 'a cat'], ['Mammalia']);
    setClasses(['a snake', 'a turtle', 'a lizard'], ['Squamata', 'Testudines', 'Reptilia']);
    setClasses(['a frog', 'a toad', 'a salamander'], ['Amphibia']);
    setClasses(['an insect', 'a dragonfly', 'a butterfly', 'a moth', 'a bee', 'a beetle'], ['Insecta']);
    setClasses(['a spider'], ['Arachnida']);
    // The queries that name a domestic animal. A box of one the detector is
    // sure of (petMinBoxScore) is ranked over its query's classes whatever
    // `candidates` says (classifyCrop's 'pet'): over every row, a dog's crop
    // could go to an insect (a dog named Eastern Cicada Killer), and a dog or
    // cat is what such a box nearly always is. Nearly: OWLv2 also calls a
    // bird a dog, so the restriction gives way both ways. At analyze() the
    // ranking over every row is the answer when it is sure (petFallback) of
    // a class outside the query's; else it is kept in the crop
    // (`unrestricted`), and labels() names the crop by it when the restricted
    // answer is not sure of a species and the photo's or burst's other boxes
    // are sure of the class the unrestricted answer leans to (petAnswer).
    const PET_QUERIES = ['a dog', 'a cat'];
    // Clear animals score 0.3 and up; real ones half hidden and look-alikes
    // share 0.1-0.3; empty frames score under 0.1. A score under
    // labelThreshold is not shown as a name (labels()).
    // A name also needs a box the detector is sure enough of (labels(), the
    // box rule): a box other than the photo's best names nothing under
    // minBoxScore; a family, order or class needs a box at broadMinBoxScore;
    // a burst name only one frame reaches (under the consensus share) needs
    // that frame's box at burstMinBoxScore; with queryMatch an order or class
    // outside its box's query's classes ("a bird" box, a mammal order) is not
    // named; with consensus, two frames or more naming one taxon on weaker
    // boxes than their best are a burst name when their share reaches the
    // threshold. Most of the order and class names weak boxes gave were wrong.
    // nms / nmsApart / nmsHold: which of the detector's boxes are kept
    // (suppress()). queryTop: the species a crop's suggestions come from
    // (analyze()'s `queryTop`, its top over its query's classes; 0 for none).
    const DEFAULTS = { model: 'bioclip-2', threshold: 0.15, topK: 5, margin: 0.15,
                       nms: 0.3, nmsApart: 0.5, nmsHold: 0.85, queryTop: 5,
                       maxDetections: 20, gapS: 1.0, labelThreshold: 0.85,
                       maxClassify: 8, candidates: 'all', petMinBoxScore: 0.3, taxaTop: 3,
                       minBoxScore: 0.3, broadMinBoxScore: 0.3, burstMinBoxScore: 0.4,
                       queryMatch: true, consensus: true };
    const MODEL_REPO = { 'bioclip': 'imageomics/bioclip', 'bioclip-2': 'imageomics/bioclip-2',
                         'bioclip-2.5': 'imageomics/bioclip-2.5-vith14',
                         'owlv2': 'google/owlv2-base-patch16-ensemble' };
    // The default species lists, merged: a file in a bro.models folder each.
    // The birds and yard mammals (eBird names), and the GBIF reptiles and
    // amphibians, arthropods and pets in folders of their own below species/.
    const SPECIES_LISTS = [
        { folder: 'species', file: 'upper-midwest.json' },
        { folder: 'species/upper-midwest-herps', file: 'species.json' },
        { folder: 'species/upper-midwest-arthropods', file: 'species.json' },
        { folder: 'species/pets', file: 'species.json' },
    ];
    const LABEL_BOX_KEYS = ['minBoxScore', 'broadMinBoxScore', 'burstMinBoxScore', 'queryMatch', 'consensus'];
    // The ranks a label can be at, narrowest first.
    const RANKS = ['species', 'genus', 'family', 'order', 'class'];
    const IMAGE_SIZE = 224;
    // open_clip's trained temperature: exp(logit_scale) = 100 for both BioCLIPs.
    const LOGIT_SCALE = 100;

    wl.QUERIES = QUERIES;
    wl.QUERY_CLASSES = QUERY_CLASSES;
    wl.PET_QUERIES = PET_QUERIES;
    wl.DEFAULTS = DEFAULTS;
    wl.SPECIES_LISTS = SPECIES_LISTS;
    wl.RANKS = RANKS;

    const fsMod = () => globalThis.require('fs');
    const slashes = (p) => String(p).replace(/\\/g, '/').replace(/\/+$/, '');
    const baseName = (p) => slashes(p).split('/').pop();
    const exists = (p) => { try { return fsMod().existsSync(p); } catch (_) { return false; } };
    const r1 = (x) => Math.round(x * 10) / 10;
    const r4 = (x) => Math.round(x * 1e4) / 1e4;

    // FNV-1a, the text-embedding cache file's name.
    function fnv1a(str) {
        let h = 0x811c9dc5;
        for (let i = 0; i < str.length; i++) {
            h ^= str.charCodeAt(i);
            h = Math.imul(h, 0x01000193) >>> 0;
        }
        return h.toString(16).padStart(8, '0');
    }

    // ── where the weights are ────────────────────────────────────────────

    /**
     * The paths open() would use and what is missing, without loading.
     * bro.models.find answers the defaults in the main realm; a Worker has no
     * bro.models, so every path must be given there. `fetch` names the
     * bro.models folders that would install what is missing of the defaults.
     */
    wl.locate = function locate(opts) {
        const o = opts || {};
        const models = globalThis.bro && globalThis.bro.models;
        const find = models && typeof models.find === 'function' ? (n) => models.find(n) : null;
        const model = o.model || DEFAULTS.model;
        const out = { detector: o.detector || null, classifier: o.classifier || null,
                      species: o.species || null, model, missing: [], fetch: [] };
        if (!find && (!o.detector || !o.classifier || !o.species)) {
            throw new Error('bro.vision.wildlife: bro.models is main-realm only; in a Worker pass ' +
                            'detector, classifier and species (paths)');
        }
        if (!out.detector) out.detector = find('owlv2');
        if (!out.classifier) out.classifier = find(model);
        if (!out.detector || !exists(out.detector + '/model.safetensors')) {
            out.missing.push('detector');
            if (!o.detector && !out.detector) out.fetch.push('owlv2');
        }
        if (!out.classifier || !exists(out.classifier + '/model.safetensors') ||
            !exists(out.classifier + '/config.json')) {
            out.missing.push('classifier');
            if (!o.classifier && !out.classifier) out.fetch.push(model);
        }
        if (!out.species) {
            // The default lists, each in its bro.models folder (null: not found).
            out.species = SPECIES_LISTS.map((l) => { const d = find(l.folder); return d ? slashes(d) + '/' + l.file : null; });
            // No species folder: fetching it brings every list (its subfolders too);
            // with it, a list folder of its own that is not there is fetched alone.
            if (!find('species')) out.fetch.push('species');
            else SPECIES_LISTS.forEach((l, i) => { if (l.folder !== 'species' && !out.species[i]) out.fetch.push(l.folder); });
        }
        // No list given and no species folder found is missing too, not "no list".
        const files = typeof out.species === 'string' ? [out.species]
            : Array.isArray(out.species) ? out.species.filter((s) => s === null || typeof s === 'string') : [];
        if (out.species == null || files.some((f) => f === null || !exists(f))) out.missing.push('species');
        return out;
    };

    /** Can open() work: the loaders are compiled in and the files are there. No weights load. */
    wl.available = function available(opts) {
        const v = globalThis.bro && globalThis.bro.vision, lm = globalThis.bro && globalThis.bro.lm;
        if (!v || typeof v.loadOwlv2 !== 'function' || !lm || typeof lm.loadClip !== 'function') return false;
        try { return wl.locate(opts).missing.length === 0; } catch (_) { return false; }
    };

    // ── species list and prompts ─────────────────────────────────────────

    /** "a photo of Animalia Chordata Aves Columbiformes Columbidae Zenaida
     *  macroura with common name Mourning Dove." (BioCLIP's Taxonomic + Common) */
    function speciesPrompt(sp) {
        const taxa = [sp.kingdom || 'Animalia', sp.phylum || 'Chordata', sp.class, sp.order,
                      sp.family, sp.scientific].filter(Boolean).join(' ');
        return `a photo of ${taxa} with common name ${sp.common}.`;
    }
    wl.speciesPrompt = speciesPrompt;

    const readDoc = (path) => JSON.parse(String(fsMod().readFileSync(path, 'utf-8')));
    // A list item that is a document ({ species, taxa? }) rather than a row.
    const isDoc = (it) => it && typeof it === 'object' && !Array.isArray(it) &&
        (Array.isArray(it.species) || (it.taxa && typeof it.taxa === 'object' && !it.scientific));

    /** The rows of a species list file ({ species: [...] } or an array). */
    wl.readSpeciesList = function readSpeciesList(path) {
        const doc = readDoc(path);
        return doc.species || doc;
    };

    // A list's name in `config`: the file's name, or for a list that is a
    // folder's species.json (the generated lists), "<folder>/species.json".
    function listName(p) {
        const parts = slashes(p).split('/');
        const base = parts.pop();
        return base === 'species.json' && parts.length ? parts.pop() + '/' + base : base;
    }

    /**
     * The rows of one list or several, merged: `lists` a file, rows, or an
     * array of files and rows. A species (by scientific name, ignoring case
     * and spacing) is kept where it first appears.
     */
    wl.readSpecies = function readSpecies(lists) {
        const items = typeof lists === 'string' ? [lists] : Array.isArray(lists) ? lists : [];
        const seen = new Set(), rows = [];
        const add = (r) => {
            const k = String(r.scientific || r.common || '').trim().replace(/\s+/g, ' ').toLowerCase();
            if (!k || seen.has(k)) return;
            seen.add(k);
            rows.push(r);
        };
        for (const it of items) {
            if (typeof it === 'string') wl.readSpeciesList(it).forEach(add);
            else if (Array.isArray(it)) it.forEach(add);
            else if (isDoc(it)) (Array.isArray(it.species) ? it.species : []).forEach(add);
            else if (it && typeof it === 'object') add(it);
        }
        return rows;
    };

    /**
     * The `taxa` maps of one list or several (as readSpecies takes them:
     * files, rows and { species, taxa } documents), merged: a scientific name
     * (ignoring case and spacing) is kept where it first appears.
     * @returns {Object<string, { rank: string, common?: string }>}
     */
    wl.readTaxa = function readTaxa(lists) {
        const items = typeof lists === 'string' ? [lists] : Array.isArray(lists) ? lists : [lists];
        const seen = new Set(), out = {};
        for (const it of items) {
            let doc = null;
            if (typeof it === 'string') { try { doc = readDoc(it); } catch (_) { doc = null; } }
            else if (isDoc(it)) doc = it;
            const taxa = doc && doc.taxa;
            if (!taxa || typeof taxa !== 'object' || Array.isArray(taxa)) continue;
            for (const name of Object.keys(taxa)) {
                const k = normName(name);
                if (!k || seen.has(k)) continue;
                seen.add(k);
                const t = taxa[name] || {};
                const v = { rank: t.rank || null };
                if (t.common) v.common = t.common;
                out[name] = v;
            }
        }
        return out;
    };

    const normName = (s) => String(s || '').trim().replace(/\s+/g, ' ').toLowerCase();

    // ── names a person gives an animal ───────────────────────────────────

    /**
     * Every name the lists know, for a person to pick from (a correction of
     * what labels() said): each species row and every taxon above species,
     * from the taxa maps (their common names, "Darners") and from the rows'
     * own lineage (the genus of a scientific name, family, order, class).
     * Plain data, so a Worker can read the lists and post it; nameIndex()
     * searches it. A taxon's lineage is the first of its rows' above it.
     * @param lists  as readSpecies takes them: files, rows, { species, taxa } documents
     * @param {{ taxa?: Object }} [opts]  a readTaxa map to use in place of the lists' own
     * @returns {Array<{ common?: string, scientific: string, rank: string,
     *                   lineage: { genus?, family?, order?, class? } }>}  species first, in list order
     */
    wl.nameEntries = function nameEntries(lists, opts) {
        const o = opts || {};
        const rows = wl.readSpecies(lists);
        const taxa = o.taxa || wl.readTaxa(lists);
        const species = [], higher = new Map();
        for (const r of rows) {
            const scientific = String(r.scientific || r.common || '').trim();
            if (!scientific) continue;
            const lineage = lineageOf(r, 'species');
            species.push(r.common ? { common: String(r.common), scientific, rank: 'species', lineage }
                                  : { scientific, rank: 'species', lineage });
            for (let i = 1; i < RANKS.length; i++) {
                const name = lineage[RANKS[i]];
                if (!name || higher.has(normName(name))) continue;
                const above = {};
                for (let j = i + 1; j < RANKS.length; j++) if (lineage[RANKS[j]]) above[RANKS[j]] = lineage[RANKS[j]];
                higher.set(normName(name), { scientific: name, rank: RANKS[i], lineage: above });
            }
        }
        for (const name of Object.keys(taxa)) {
            const t = taxa[name] || {};
            const have = higher.get(normName(name));
            if (have) { if (t.common) have.common = String(t.common); continue; }
            if (RANKS.indexOf(t.rank) < 1) continue;
            higher.set(normName(name), { scientific: name, rank: t.rank, lineage: {} });
            if (t.common) higher.get(normName(name)).common = String(t.common);
        }
        const out = species.slice();
        for (const e of higher.values()) {
            out.push(e.common ? { common: e.common, scientific: e.scientific, rank: e.rank, lineage: e.lineage }
                              : e);
        }
        return out;
    };

    // Text as names are matched: no case, no accents, no apostrophes, any
    // other run of punctuation or space one space ("Cooper's Hawk" ->
    // "coopers hawk", "Lance-tipped" -> "lance tipped").
    function foldName(s) {
        let t = String(s == null ? '' : s).toLowerCase();
        if (typeof t.normalize === 'function') t = t.normalize('NFD').replace(/[̀-ͯ]/g, '');
        return t.replace(/['’`]/g, '').replace(/[^a-z0-9]+/g, ' ').trim();
    }

    // Edit distance of a and b, or limit + 1 once it is over `limit`.
    function editDistance(a, b, limit) {
        if (Math.abs(a.length - b.length) > limit) return limit + 1;
        let prev = [];
        for (let j = 0; j <= b.length; j++) prev.push(j);
        for (let i = 1; i <= a.length; i++) {
            const cur = [i];
            let best = i;
            for (let j = 1; j <= b.length; j++) {
                const v = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
                cur.push(v);
                if (v < best) best = v;
            }
            if (best > limit) return limit + 1;
            prev = cur;
        }
        return prev[b.length];
    }

    // How well the folded query `q` (its words `qw`) matches a folded name
    // `f` (its words `w`): 100 the name, 90 its start, 80 each word the
    // start of a name's word in order, 70 in any order, 60 inside it (3
    // letters or more), 50 each word of 4 letters or more one typo from a
    // word's start (shorter ones exact); 0 no match.
    function matchTier(q, qw, f, w) {
        if (!q || !f) return 0;
        if (f === q) return 100;
        if (f.startsWith(q)) return 90;
        let j = 0, inOrder = true;
        for (const x of qw) {
            while (j < w.length && !w[j].startsWith(x)) j++;
            if (j >= w.length) { inOrder = false; break; }
            j++;
        }
        if (inOrder) return 80;
        const used = new Set();
        const take = (fits) => {
            for (let k = 0; k < w.length; k++) if (!used.has(k) && fits(w[k])) { used.add(k); return true; }
            return false;
        };
        if (qw.every((x) => take((y) => y.startsWith(x)))) return 70;
        if (q.length >= 3 && f.includes(q)) return 60;
        used.clear();
        const near = (x) => (y) => {
            if (y.startsWith(x)) return true;
            if (x.length < 4) return false;
            for (let n = x.length - 1; n <= x.length + 1; n++) {
                if (n <= y.length && editDistance(x, y.slice(0, n), 1) <= 1) return true;
            }
            return false;
        };
        if (qw.every((x) => take(near(x)))) return 50;
        return 0;
    }

    /**
     * A search over nameEntries(), for an input that suggests names as a
     * person types: `search(text, { limit })` -> the best entries first,
     * each { ...entry, label (formatLabel), score, on: 'common' |
     * 'scientific' }, matched by common and scientific name, case, accents
     * and punctuation aside: the whole name, its start, the start of each of
     * its words (in order, then any order: "gr eg" finds Great Egret), inside
     * it, then a typo a word ("malard"). Ties go to a common name, then a
     * species, then the shorter. `find(text)` -> the entry whose common name,
     * scientific name or label is the text, or null (a species first).
     * @param {Array} entries  nameEntries()'
     */
    wl.nameIndex = function nameIndex(entries) {
        const list = Array.isArray(entries) ? entries.filter((e) => e && e.scientific) : [];
        const items = list.map((e, i) => {
            const label = wl.formatLabel(e);
            const keys = [];
            if (e.common) keys.push({ on: 'common', f: foldName(e.common) });
            keys.push({ on: 'scientific', f: foldName(e.scientific) });
            for (const k of keys) k.w = k.f.split(' ').filter(Boolean);
            return { e, i, label, keys };
        });
        const exact = new Map();
        const put = (text, it) => { const k = foldName(text); if (k && !exact.has(k)) exact.set(k, it); };
        for (const pass of [true, false]) {
            for (const it of items) {
                if ((it.e.rank === 'species') !== pass) continue;
                if (it.e.common) put(it.e.common, it);
                put(it.e.scientific, it);
                put(it.label, it);
            }
        }
        const result = (it, score, on) => Object.assign({}, it.e, { label: it.label, score, on });
        return {
            entries: list,
            size: list.length,
            find(text) {
                const it = exact.get(foldName(text));
                return it ? result(it, 100, it.e.common && foldName(it.e.common) === foldName(text) ? 'common' : 'scientific') : null;
            },
            search(text, opts) {
                const limit = opts && opts.limit > 0 ? opts.limit : 12;
                const q = foldName(text);
                if (!q) return [];
                const qw = q.split(' ');
                const hits = [];
                for (const it of items) {
                    let best = 0, on = '';
                    for (const k of it.keys) {
                        const t = matchTier(q, qw, k.f, k.w);
                        if (t > best) { best = t; on = k.on; }
                    }
                    if (best) hits.push({ it, best, on });
                }
                hits.sort((a, b) => b.best - a.best || (a.on === b.on ? 0 : a.on === 'common' ? -1 : 1) ||
                    rankIx(a.it.e.rank) - rankIx(b.it.e.rank) || a.it.label.length - b.it.label.length ||
                    (a.it.label < b.it.label ? -1 : a.it.label > b.it.label ? 1 : a.it.i - b.it.i));
                return hits.slice(0, limit).map((h) => result(h.it, h.best, h.on));
            },
        };
    };

    /**
     * The rows of `matrix` most like `query`: [{ index, similarity }] best
     * first (ties by index), at most `n` (default 1), none under
     * opts.minSimilarity. `query` is a Float32Array (or array) of dim
     * numbers; `matrix` is the rows end to end (a Float32Array, length a
     * multiple of dim) or an array of rows. The similarity is the dot product
     * (cosine for unit vectors, as analyze()'s embeddings are), summed as
     * classifyCrop's are (__bro_native.vision.dotRows). Bad shapes answer [].
     */
    wl.nearest = function nearest(query, matrix, n, opts) {
        const o = opts || {};
        const dim = query ? query.length : 0;
        if (!dim) return [];
        let rows = matrix;
        if (Array.isArray(matrix)) {
            rows = new Float32Array(matrix.length * dim);
            for (let i = 0; i < matrix.length; i++) {
                if (!matrix[i] || matrix[i].length !== dim) return [];
                rows.set(matrix[i], i * dim);
            }
        } else if (!(matrix instanceof Float32Array)) rows = matrix ? Float32Array.from(matrix) : null;
        if (!rows || !rows.length || rows.length % dim) return [];
        const q = query instanceof Float32Array ? query : Float32Array.from(query);
        const sims = __bro_native.vision.dotRows(q, rows, dim);
        const min = o.minSimilarity == null ? -Infinity : o.minSimilarity;
        const want = n == null ? 1 : Math.max(0, Math.floor(n));
        const out = [];
        for (let i = 0; i < sims.length; i++) if (sims[i] >= min) out.push({ index: i, similarity: sims[i] });
        out.sort((a, b) => b.similarity - a.similarity || a.index - b.index);
        return out.slice(0, want);
    };

    // ── taxa, ranks and labels ───────────────────────────────────────────

    // A row's or entry's lineage above `rank`: { genus?, family?, order?, class? }
    // (a species' genus is the first word of its scientific name).
    function lineageOf(e, rank) {
        const out = {};
        const from = RANKS.indexOf(rank || 'species');
        for (let i = from + 1; i < RANKS.length; i++) {
            const r = RANKS[i];
            let v = e[r];
            if (r === 'genus' && !v && from === 0 && e.scientific) v = String(e.scientific).trim().split(/\s+/)[0];
            if (v) out[r] = v;
        }
        return out;
    }
    wl.lineage = (row) => lineageOf(row, 'species');
    // A species row's lineage, worked out once a row (taxaScores runs over
    // every row of the list for every crop): { lin, ids }, ids a number per
    // rank above species (genus, family, order, class; -1 for none), each
    // name's index in TAXON_NAMES[rank]. Keyed by the row object, keeping the
    // field values it was worked out from, so an edited row is redone.
    const LINEAGES = new WeakMap();
    const TAXON_IDS = RANKS.slice(1).map(() => new Map());
    const TAXON_NAMES = RANKS.slice(1).map(() => []);
    function rowLineage(row) {
        const c = LINEAGES.get(row);
        if (c && c.scientific === row.scientific && c.genus === row.genus && c.family === row.family &&
            c.order === row.order && c.class === row.class) return c;
        const lin = lineageOf(row, 'species');
        const ids = RANKS.slice(1).map((r, k) => {
            const name = lin[r];
            if (!name) return -1;
            let id = TAXON_IDS[k].get(name);
            if (id === undefined) {
                id = TAXON_NAMES[k].length;
                TAXON_IDS[k].set(name, id);
                TAXON_NAMES[k].push(name);
            }
            return id;
        });
        const e = { scientific: row.scientific, genus: row.genus, family: row.family,
                    order: row.order, class: row.class, lin, ids };
        LINEAGES.set(row, e);
        return e;
    }

    /**
     * The probability of every genus, family, order and class: the sum of
     * `probs[i]` over the rows `rows[i]` in it (a row without a rank adds
     * nothing there). Pure; `probs` any numbers parallel to `rows`.
     * @param {Object[]} rows   species rows (or top-K entries)
     * @param {ArrayLike<number>} probs
     * @param {{top?: number}} [opts]  entries per rank (default 3; 0 for all)
     * @returns {{ genus, family, order, class: Array<{ scientific, score,
     *   family?, order?, class? }> }}  best first, each with its lineage above it
     */
    wl.taxaScores = function taxaScores(rows, probs, opts) {
        const top = opts && opts.top != null ? opts.top : DEFAULTS.taxaTop;
        // Each taxon's score summed in row order, from 0 (a taxon no row with
        // p > 0 is in has no entry), its lineage the first such row's.
        const lins = new Array(rows.length);
        for (let i = 0; i < rows.length; i++) {
            const row = rows[i], p = probs[i];
            if (row && p > 0) lins[i] = rowLineage(row);
        }
        const nRanks = RANKS.length - 1;
        const score = [], first = [], touched = [];
        for (let k = 0; k < nRanks; k++) {
            score.push(new Float64Array(TAXON_NAMES[k].length));
            first.push(new Int32Array(TAXON_NAMES[k].length).fill(-1));
            touched.push([]);
        }
        for (let i = 0; i < rows.length; i++) {
            const e = lins[i];
            if (!e) continue;
            const p = probs[i];
            for (let k = 0; k < nRanks; k++) {
                const id = e.ids[k];
                if (id < 0) continue;
                if (first[k][id] < 0) { first[k][id] = i; touched[k].push(id); }
                score[k][id] += p;
            }
        }
        const out = {};
        for (let k = 0; k < nRanks; k++) {
            const r = RANKS[k + 1], s = score[k], names = TAXON_NAMES[k];
            // Best first, a tie to the name first in code-unit order.
            const before = (a, b) => s[a] > s[b] || (s[a] === s[b] && names[a] < names[b]);
            let ids = touched[k];
            if (top && top > 0 && ids.length > top) {
                const best = [];
                for (const id of ids) {
                    if (best.length >= top && !before(id, best[best.length - 1])) continue;
                    let at = best.length;
                    while (at > 0 && before(id, best[at - 1])) at--;
                    best.splice(at, 0, id);
                    if (best.length > top) best.pop();
                }
                ids = best;
            } else {
                ids = ids.slice().sort((a, b) => (before(a, b) ? -1 : before(b, a) ? 1 : 0));
            }
            out[r] = ids.map((id) => Object.assign({ scientific: names[id], score: s[id] },
                                                   lineageOf(lins[first[k][id]].lin, r)));
        }
        return out;
    };

    /** The classes a query's crop is ranked over ('byQuery'), or null for every species. */
    wl.queryClasses = (query) => QUERY_CLASSES[query] || null;

    // Softmax at the model's temperature over `idx` (indices into sims).
    function softmaxOver(sims, idx) {
        let max = -Infinity;
        for (const i of idx) if (sims[i] > max) max = sims[i];
        const p = new Float64Array(idx.length);
        let z = 0;
        for (let k = 0; k < idx.length; k++) { p[k] = Math.exp((sims[idx[k]] - max) * LOGIT_SCALE); z += p[k]; }
        for (let k = 0; k < idx.length; k++) p[k] /= z;
        return p;
    }

    /**
     * One crop's ranking from its cosine similarity to every species prompt.
     * Pure: what analyze() does per box, testable without a model (a
     * similarity of log(p) / 100 is a softmax probability p).
     * `candidates` 'all': the softmax is over every row. 'byQuery': over the
     * rows whose class is one of queryClasses(query) (every row when it has
     * none or none is listed), except that when the best species there
     * scores under `fallback` while the best over every row scores at least
     * that and is of another class, the ranking over every row is the answer.
     * 'query': over those rows with no fallback. 'pet' (analyze() ranks a
     * sure pet box this way, petMinBoxScore): over those rows, unless the
     * ranking over every row gives a class outside them `fallback` or more
     * (its best species there, or its best class), when that ranking is the
     * answer ('fallback'); kept to the query's classes, the ranking over
     * every row comes with it as `unrestricted` ({ species, taxa }).
     * @param {Object[]} rows   the species rows
     * @param {ArrayLike<number>} sims  cosine similarity per row
     * @param {{ topK?, top?, query?, candidates?: 'all'|'byQuery'|'query'|'pet', fallback? }} [opts]
     * @returns {{ species: Array<{ common, scientific, family, order, class,
     *   score, similarity }>, taxa: (taxaScores), candidates: 'all'|'query'|'fallback',
     *   unrestricted?: { species, taxa } }}  unrounded
     */
    wl.classifyCrop = function classifyCrop(rows, sims, opts) {
        const o = opts || {};
        const topK = o.topK || DEFAULTS.topK;
        const fallback = o.fallback != null ? o.fallback : DEFAULTS.labelThreshold;
        const all = Array.from({ length: rows.length }, (_, i) => i);
        const rankOver = (idx) => {
            const p = softmaxOver(sims, idx);
            // The top K by similarity, ties to the earlier row: what sorting
            // every row would put first, without sorting ~2000 of them.
            const order = [];
            for (let k = 0; k < idx.length; k++) {
                const s = sims[idx[k]];
                if (order.length >= topK && !(s > sims[idx[order[order.length - 1]]])) continue;
                let at = order.length;
                while (at > 0 && s > sims[idx[order[at - 1]]]) at--;
                order.splice(at, 0, k);
                if (order.length > topK) order.pop();
            }
            const species = order.map((k) => {
                const sp = rows[idx[k]];
                return { common: sp.common, scientific: sp.scientific, order: sp.order,
                         family: sp.family, class: sp.class, score: p[k], similarity: sims[idx[k]] };
            });
            return { species, taxa: wl.taxaScores(idx.map((i) => rows[i]), p, { top: o.top }) };
        };
        const mode = o.candidates || DEFAULTS.candidates;
        const classes = mode === 'byQuery' || mode === 'query' || mode === 'pet' ? wl.queryClasses(o.query) : null;
        if (classes) {
            const sub = all.filter((i) => classes.includes(rows[i].class));
            if (sub.length && sub.length < all.length) {
                const q = rankOver(sub);
                if (mode === 'pet') {
                    // Sure of something else over every row: that is the answer.
                    const a = rankOver(all);
                    const sp = a.species[0], cl = a.taxa.class[0];
                    if ((sp && sp.score >= fallback && !classes.includes(sp.class)) ||
                        (cl && cl.score >= fallback && !classes.includes(cl.scientific))) {
                        return Object.assign(a, { candidates: 'fallback' });
                    }
                    return Object.assign(q, { candidates: 'query', unrestricted: { species: a.species, taxa: a.taxa } });
                }
                if (mode === 'query' || q.species[0].score >= fallback) return Object.assign(q, { candidates: 'query' });
                const a = rankOver(all);
                if (a.species[0].score >= fallback && !classes.includes(a.species[0].class)) {
                    return Object.assign(a, { candidates: 'fallback' });
                }
                return Object.assign(q, { candidates: 'query' });
            }
        }
        return Object.assign(rankOver(all), { candidates: 'all' });
    };

    /**
     * The classifyCrop `candidates` analyze() ranks a box with: 'pet' for
     * a box of a pet query (PET_QUERIES) scoring petMinBoxScore or more,
     * else the analysis mode ('all' or 'byQuery'). Pure.
     * @param {string} query     the box's detector query ('a dog')
     * @param {number} boxScore  its detector score (the record's, 4 places)
     * @param {{ mode?: 'all'|'byQuery', petMinBoxScore?: number|null }} [mode]
     *   a config().candidates; the defaults where not given, null for no pet rule
     */
    wl.candidatesFor = function candidatesFor(query, boxScore, mode) {
        const m = mode || {};
        const pet = m.petMinBoxScore === undefined ? DEFAULTS.petMinBoxScore : m.petMinBoxScore;
        if (pet != null && PET_QUERIES.includes(query) && boxScore >= pet) return 'pet';
        return m.mode || DEFAULTS.candidates;
    };

    /**
     * A crop's suggestions: its top `n` species over the rows of its query's
     * classes (queryClasses; every row for a query without them or when no
     * row is of them), the softmax over those rows alone (classifyCrop's
     * 'query'), so a box the detector called a bird suggests only birds.
     * analyze() keeps it as an animal's `queryTop`, rounded. Pure.
     * @returns {Array<{ common, scientific, family, order, class, score, similarity }>}  best first, unrounded
     */
    wl.queryTop = function queryTop(rows, sims, query, n) {
        return wl.classifyCrop(rows, sims, { topK: n || DEFAULTS.queryTop, query, candidates: 'query' }).species;
    };

    /**
     * Greedy suppression of detector boxes, best first. A box is dropped
     * against a better box kept before it (any query) when:
     *  - their IoU is over `apart`: one animal boxed twice;
     *  - their IoU is over `iou` and half of the box or more lies inside the
     *    better one: a part of that animal, or the same one again;
     *  - it is the bigger and holds `hold` or more of the better one: a box
     *    over that animal and more (a group, its surroundings).
     * Two animals that overlap (IoU over `iou`, up to `apart`) keep their
     * boxes when the lower one reaches mostly outside the better one and
     * does not hold it: a duck half behind another. With `apart` and `hold`
     * null it is OWLv2's own NMS at `iou` (what detect() then runs natively).
     * At most `max` kept. Pure.
     * @param {Array<{ box: number[], score: number, label?: string }>} hits  best first, boxes [x, y, w, h]
     * @param {{ iou?: number, apart?: number|null, hold?: number|null, max?: number }} [opts]
     *   iou 0 or >= 1: no IoU test; the defaults DEFAULTS' nms, nmsApart, nmsHold
     * @returns the hits kept, in order
     */
    wl.suppress = function suppress(hits, opts) {
        const o = opts || {};
        const iou = o.iou == null ? DEFAULTS.nms : o.iou;
        const apart = o.apart === undefined ? DEFAULTS.nmsApart : o.apart;
        const hold = o.hold === undefined ? DEFAULTS.nmsHold : o.hold;
        const max = o.max == null ? Infinity : o.max;
        const byIou = iou > 0 && iou < 1;
        const hi = apart == null ? iou : Math.max(iou, apart);
        const keep = [];
        for (const d of hits) {
            if (keep.length >= max) break;
            const [x, y, w, h] = d.box, ad = w * h;
            let drop = false;
            for (const k of keep) {
                const [kx, ky, kw, kh] = k.box;
                const iw = Math.min(x + w, kx + kw) - Math.max(x, kx);
                const ih = Math.min(y + h, ky + kh) - Math.max(y, ky);
                if (iw <= 0 || ih <= 0) continue;
                const inter = iw * ih, ak = kw * kh;
                const uni = ad + ak - inter;
                const ov = uni > 0 ? inter / uni : 0;
                if (byIou && (ov > hi || (ov > iou && inter >= 0.5 * ad))) { drop = true; break; }
                if (hold != null && ad > ak && inter >= hold * ak) { drop = true; break; }
            }
            if (!drop) keep.push(d);
        }
        return keep;
    };

    // A record's crops: its `animals`, or for a record from before them, its
    // top K as the one crop (taxa summed over the top K), on the best box.
    // Each with `primary` (the photo's best box) and its detector `score`
    // and query `label` when the record has them.
    // With `pets` ({ t, classes }), a pet crop is labelled by petAnswer().
    function cropsOf(rec, pets) {
        if (!rec || rec.error || !rec.animal) return [];
        if (Array.isArray(rec.animals)) {
            const crops = rec.animals.map((a, i) => (a && a.species && a.species.length
                ? Object.assign({}, a, { primary: i === 0 }) : null)).filter(Boolean);
            return pets ? crops.map((c) => petAnswer(c, pets.t, pets.classes)) : crops;
        }
        if (!rec.species || !rec.species.length) return [];
        const d = rec.detections && rec.detections[0];
        return [{ box: d ? d.box : null, score: d ? d.score : undefined, label: d ? d.label : undefined,
                  primary: true, species: rec.species,
                  taxa: wl.taxaScores(rec.species, rec.species.map((s) => s.score), { top: 0 }) }];
    }

    // ── which boxes may name what ────────────────────────────────────────

    // The box rule of labels(): opts over DEFAULTS.
    function boxRule(o) {
        const pick = (k) => (o[k] != null ? o[k] : DEFAULTS[k]);
        return { minBox: pick('minBoxScore'), broadMin: pick('broadMinBoxScore'),
                 burstMin: pick('burstMinBoxScore'), queryMatch: !!pick('queryMatch'),
                 consensus: !!pick('consensus') };
    }

    // Is the taxon `e` (at `rank`, with its lineage) in one of a query's classes?
    // A query without classes, or a box without a query, matches anything.
    function queryMatches(query, rank, e) {
        const classes = query ? QUERY_CLASSES[query] : null;
        if (!classes) return true;
        if (classes.includes(e.scientific)) return true;
        const lin = e.lineage || lineageOf(e, rank);
        return RANKS.some((r) => lin[r] && classes.includes(lin[r]));
    }

    // May a box (detector score `box`, missing = sure; `primary`: the photo's
    // best; `match`: its query matches the taxon) name a taxon at `rank`?
    function boxMayName(rule, rank, box, primary, match) {
        const s = box == null ? 1 : box;
        if (!primary && s < rule.minBox) return false;
        if (rankIx(rank) >= rankIx('family') && s < rule.broadMin) return false;
        if (rule.queryMatch && rankIx(rank) >= rankIx('order') && !match) return false;
        return true;
    }
    // The rule for a burst consensus: any box of a frame, not only its best, may name it.
    const looseRule = (rule) => Object.assign({}, rule, { minBox: 0 });
    const cropMayName = (rule, crop, rank, e) =>
        boxMayName(rule, rank, crop.score, crop.primary, queryMatches(crop.label, rank, e));

    // Every scored taxon of a crop, by rank: [{ rank, scientific, common?, score, lineage }].
    function cropEntries(crop) {
        const out = [];
        for (const s of crop.species || []) {
            out.push({ rank: 'species', scientific: s.scientific, common: s.common, score: s.score,
                       lineage: lineageOf(s, 'species') });
        }
        const taxa = crop.taxa || wl.taxaScores(crop.species || [], (crop.species || []).map((s) => s.score), { top: 0 });
        for (const r of RANKS.slice(1)) {
            for (const t of taxa[r] || []) {
                out.push({ rank: r, scientific: t.scientific, score: t.score, lineage: lineageOf(t, r) });
            }
        }
        return out;
    }

    // A crop's best guess at a rank (its top-1 there), or null.
    function cropTop(crop, rank) {
        if (rank === 'species') return crop.species && crop.species[0] || null;
        const taxa = crop.taxa || wl.taxaScores(crop.species || [], (crop.species || []).map((s) => s.score), { top: 0 });
        return taxa[rank] && taxa[rank][0] || null;
    }

    /**
     * A crop's label: the narrowest rank whose best score is at least `t`
     * and which its box may name (boxMayName), or null (found, not identified).
     */
    function cropLabel(crop, t, rule) {
        for (const r of RANKS) {
            const e = cropTop(crop, r);
            if (e && e.score >= t && cropMayName(rule, crop, r, e)) {
                return { rank: r, scientific: e.scientific, common: r === 'species' ? e.common : undefined,
                         score: e.score, lineage: lineageOf(e, r) };
            }
        }
        return null;
    }

    // ── pet boxes the rest of the photo or burst says are something else ──

    // A sure pet box's crop kept to its query's classes, with the answer
    // over every species beside it (analyze()'s `unrestricted`).
    const petKept = (c) => c.candidates === 'query' && !!c.unrestricted &&
        Array.isArray(c.unrestricted.species) && c.unrestricted.species.length > 0;

    /**
     * The classes a photo's or burst's crops are sure of, leaving out the
     * pet crops kept to their query's classes: a crop's best class scoring
     * `t` or more on a box at `broadMin` or more (the bar a class name
     * needs). Sorted. Pure.
     */
    function sureClasses(crops, t, broadMin) {
        const out = [];
        for (const c of crops) {
            if (petKept(c) || (c.score == null ? 1 : c.score) < broadMin) continue;
            const e = cropTop(c, 'class');
            if (e && e.score >= t && !out.includes(e.scientific)) out.push(e.scientific);
        }
        return out.sort();
    }

    /**
     * The answer a crop is labelled with: its own, except that a pet crop
     * kept to its query's classes whose best species there scores under `t`
     * is labelled by its answer over every species when that answer's best
     * class is one `classes` (sureClasses of the rest of the photo or
     * burst) holds and the query's classes do not. The restriction stands
     * when it is sure of a species, or when nothing else in the photo or
     * burst is sure of the class the box leans to.
     */
    function petAnswer(crop, t, classes) {
        if (!petKept(crop) || !classes || !classes.length) return crop;
        if (crop.species[0] && crop.species[0].score >= t) return crop;
        const u = crop.unrestricted;
        const uc = cropTop(u, 'class');
        const own = QUERY_CLASSES[crop.label] || [];
        if (!uc || own.includes(uc.scientific) || !classes.includes(uc.scientific)) return crop;
        return Object.assign({}, crop, { species: u.species, taxa: u.taxa, candidates: 'unrestricted' });
    }

    /** sureClasses and petAnswer for tests: (crops, t, broadMin) and (crop, t, classes). */
    wl.sureClasses = (crops, t, broadMin) => sureClasses(crops,
        t == null ? DEFAULTS.labelThreshold : t, broadMin == null ? DEFAULTS.broadMinBoxScore : broadMin);
    wl.petAnswer = (crop, t, classes) => petAnswer(crop, t == null ? DEFAULTS.labelThreshold : t, classes);

    const rankIx = (r) => RANKS.indexOf(r);
    const boxKey = (b) => JSON.stringify(b);
    function addBoxes(to, boxes) {
        const have = new Set(to.map(boxKey));
        for (const b of boxes) if (b && !have.has(boxKey(b))) { to.push(b); have.add(boxKey(b)); }
    }

    // Drop a label when a narrower one in the same answer lies inside it;
    // its boxes join that one when it is the only one.
    function dropBroader(labels) {
        const alive = new Set(labels);
        const byRank = labels.slice().sort((a, b) => rankIx(a.rank) - rankIx(b.rank));
        for (const L of byRank) {
            if (L.rank === 'species') continue;
            const inside = byRank.filter((N) => N !== L && alive.has(N) && rankIx(N.rank) < rankIx(L.rank) &&
                N.lineage[L.rank] === L.scientific);
            if (!inside.length) continue;
            alive.delete(L);
            if (inside.length === 1) addBoxes(inside[0].boxes, L.boxes);
        }
        return labels.filter((L) => alive.has(L));
    }

    function taxonCommon(taxa, scientific) {
        if (!taxa) return null;
        const t = taxa[scientific];
        if (t) return t.common || null;
        const k = normName(scientific);
        for (const name of Object.keys(taxa)) if (normName(name) === k) return taxa[name].common || null;
        return null;
    }

    // The public shape, best first.
    function finish(labels, from, taxa) {
        return labels.map((L) => {
            const out = { common: L.rank === 'species' ? (L.common || L.scientific)
                                                      : (taxonCommon(taxa, L.scientific) || L.scientific),
                          scientific: L.scientific, rank: L.rank, score: r4(L.score), from, boxes: L.boxes };
            if (from === 'burst') { out.frames = L.frames; out.share = L.share; }
            return out;
        }).sort((a, b) => b.score - a.score || rankIx(a.rank) - rankIx(b.rank) ||
            (a.scientific < b.scientific ? -1 : 1));
    }

    function photoLabels(rec, t, rule, pets) {
        const groups = new Map();
        for (const crop of cropsOf(rec, pets)) {
            const l = cropLabel(crop, t, rule);
            if (!l) continue;
            const k = l.rank + '|' + l.scientific;
            const g = groups.get(k);
            if (!g) { l.boxes = crop.box ? [crop.box] : []; groups.set(k, l); }
            else {
                g.score = Math.max(g.score, l.score);
                if (crop.box) addBoxes(g.boxes, [crop.box]);
            }
        }
        return dropBroader(Array.from(groups.values()));
    }

    // A taxon's evidence in one burst frame: its crops as flat
    // [score, boxScore, flags] triples (flags: 1 the photo's best box, 2 the
    // box's query matches the taxon), a crop another beats on all three left out.
    const EV_PRIMARY = 1, EV_MATCH = 2;
    function addEvidence(list, score, box, flags) {
        const dominates = (s1, b1, f1, s2, b2, f2) => s1 >= s2 && b1 >= b2 && (f1 & f2) === f2;
        for (let i = 0; i < list.length; i += 3) {
            if (dominates(list[i], list[i + 1], list[i + 2], score, box, flags)) return;
        }
        for (let i = list.length - 3; i >= 0; i -= 3) {
            if (dominates(score, box, flags, list[i], list[i + 1], list[i + 2])) list.splice(i, 3);
        }
        list.push(score, box, flags);
    }

    // One frame's score for a taxon under the box rule, and the best box
    // among the crops that reach `t`.
    function frameScore(rule, rank, ev, t) {
        let score = 0, box = -1;
        for (let i = 0; i < ev.length; i += 3) {
            if (!boxMayName(rule, rank, ev[i + 1], (ev[i + 2] & EV_PRIMARY) !== 0, (ev[i + 2] & EV_MATCH) !== 0)) continue;
            if (ev[i] > score) score = ev[i];
            if (ev[i] >= t && ev[i + 1] > box) box = ev[i + 1];
        }
        return { score, box };
    }

    // A burst's labels depend only on the burst, `t` and the rule, and every
    // frame of it asks for them (labels() once a photo): weighing its
    // evidence is O(frames) a call, so a burst of N frames labelled frame by
    // frame was O(N^2). Kept per burst object (groupBursts makes new ones)
    // and handed out as copies, since frameBoxes fills in their boxes.
    const burstLabelCache = new WeakMap();
    function burstLabels(burst, t, rule) {
        const key = t + '|' + rule.minBox + '|' + rule.broadMin + '|' + rule.burstMin + '|' +
            rule.queryMatch + '|' + rule.consensus;
        let byKey = burstLabelCache.get(burst);
        if (!byKey) { byKey = new Map(); burstLabelCache.set(burst, byKey); }
        let ls = byKey.get(key);
        if (!ls) { ls = weighBurst(burst, t, rule); byKey.set(key, ls); }
        return ls.map((L) => Object.assign({}, L, { boxes: L.boxes.slice() }));
    }

    function weighBurst(burst, t, rule) {
        if (!Array.isArray(burst.taxa)) {
            // A burst from before per-taxon consensus: its one species.
            const s = burst.species;
            return s && s.share >= t ? [{ rank: 'species', scientific: s.scientific, common: s.common,
                score: s.share, share: s.share, frames: s.agree, lineage: {}, boxes: [] }] : [];
        }
        const out = [];
        for (const T of burst.taxa) {
            let scores = T.scores || [], share = T.share, box = 1, loose = false;
            if (Array.isArray(T.evidence) && burst.animalCount > 0) {
                // Each frame's score from the crops the box rule lets name it.
                const fs = T.evidence.map((ev) => frameScore(rule, T.rank, ev, t));
                scores = fs.map((f) => f.score).sort((a, b) => b - a);
                share = round(scores.reduce((s, v) => s + v, 0) / burst.animalCount, 4);
                box = Math.max(-1, ...fs.filter((f) => f.score >= t).map((f) => f.box));
                if (rule.consensus && !(share >= t)) {
                    // Frames agreeing on a box other than their best, too weak
                    // alone: a consensus (share at t) with two frames or more at t.
                    const ls = T.evidence.map((ev) => frameScore(looseRule(rule), T.rank, ev, t).score);
                    const lshare = round(ls.reduce((s, v) => s + v, 0) / burst.animalCount, 4);
                    if (lshare >= t && ls.filter((s) => s >= t).length >= 2) {
                        share = lshare;
                        scores = ls.sort((a, b) => b - a);
                        loose = true;
                    }
                }
            }
            const frames = scores.filter((s) => s >= t).length;
            if (!(share >= t) && !frames) continue;
            // One frame's name, not the consensus: that frame's box must be sure.
            if (!(share >= t) && frames === 1 && box < rule.burstMin) continue;
            out.push({ rank: T.rank, scientific: T.scientific, common: T.common, share,
                       score: Math.max(share, scores[0] || 0), frames,
                       lineage: lineageOf(T, T.rank), boxes: [], loose });
        }
        return dropBroader(out);
    }

    // A frame's boxes for its burst's labels: each crop goes to the
    // narrowest label that is its own best guess at that label's rank and
    // that its box may name.
    function frameBoxes(labels, rec, rule, pets) {
        const ordered = labels.slice().sort((a, b) => rankIx(a.rank) - rankIx(b.rank));
        for (const crop of cropsOf(rec, pets)) {
            if (!crop.box) continue;
            for (const L of ordered) {
                const e = cropTop(crop, L.rank);
                if (e && e.scientific === L.scientific && cropMayName(L.loose ? looseRule(rule) : rule, crop, L.rank, e)) {
                    addBoxes(L.boxes, [crop.box]);
                    break;
                }
            }
        }
    }

    /**
     * The names a photo or burst is shown with, best first ([] for none).
     * A photo in a burst whose labels are not empty shows the burst's
     * (from 'burst', with this photo's own boxes for each); else its own
     * (from 'photo'). A crop is named at the narrowest rank (species, genus,
     * family, order, class) whose best score is at least `labelThreshold`;
     * crops of one taxon are one label (the best score, every box), and a
     * label with a narrower one inside it is left out. labels(null, burst)
     * is the burst's own. The box rule (DEFAULTS' comment) leaves out what
     * weak boxes name: a box other than the photo's best under minBoxScore,
     * a family and up under broadMinBoxScore, a burst name one frame
     * reaches on a box under burstMinBoxScore, an order or class outside
     * the box's query's classes (queryMatch); a burst without `evidence`
     * (grouped before the rule) is weighed by its scores alone. A pet crop
     * kept to its query's classes is named by its answer over every species
     * when petAnswer says so against the burst's `sureClasses` (a burst
     * grouped before them, or no burst: the photo's own crops').
     * @param {Object|null} rec
     * @param {Object|null} [burst]
     * @param {{ labelThreshold?: number, taxa?: Object, minBoxScore?: number,
     *   broadMinBoxScore?: number, burstMinBoxScore?: number, queryMatch?: boolean,
     *   consensus?: boolean }} [opts]  `taxa`: readTaxa's map, the common
     *   names of genera and up; the box rule's options default to DEFAULTS'
     * @returns {Array<{ common, scientific, rank, score, from, boxes,
     *                   frames?, share? }>}
     */
    wl.labels = function labels(rec, burst, opts) {
        const o = opts || {};
        const t = o.labelThreshold != null ? o.labelThreshold : DEFAULTS.labelThreshold;
        const rule = boxRule(o);
        // What a pet crop kept to its query's classes is weighed against:
        // the burst's sure classes (groupBursts), else the photo's own.
        const pets = { t, classes: burst && Array.isArray(burst.sureClasses) ? burst.sureClasses
            : rec ? sureClasses(cropsOf(rec), t, rule.broadMin) : [] };
        if (burst) {
            const bl = burstLabels(burst, t, rule);
            if (bl.length) {
                if (rec) frameBoxes(bl, rec, rule, pets);
                return finish(bl, 'burst', o.taxa);
            }
        }
        return rec ? finish(photoLabels(rec, t, rule, pets), 'photo', o.taxa) : [];
    };

    /**
     * The one name to show: labels()[0], or null (no animal, or none
     * identified at `labelThreshold`).
     */
    wl.label = function label(rec, burst, opts) {
        return wl.labels(rec, burst, opts)[0] || null;
    };

    /** "Mallard"; "Darners (Aeshnidae)" for a rank above species without its own name, the scientific alone. */
    wl.formatLabel = function formatLabel(l) {
        if (!l) return '';
        if (l.rank === 'species' || !l.rank) return l.common || l.scientific;
        return l.common && l.common !== l.scientific ? `${l.common} (${l.scientific})` : l.scientific;
    };

    // What a model directory is: upstream repo + revision (its ATTRIBUTION.md)
    // and the weights file's size.
    function modelInfo(name, dir) {
        const info = { name, repo: MODEL_REPO[name] || null, revision: null, weightsBytes: null };
        if (!dir) return info;
        const fs = fsMod();
        try { info.weightsBytes = fs.statSync(dir + '/model.safetensors').size; } catch (_) {}
        try {
            const att = String(fs.readFileSync(dir + '/ATTRIBUTION.md', 'utf-8'));
            const rev = /revision\s+`([0-9a-f]{7,40})`/i.exec(att);
            if (rev) info.revision = rev[1];
        } catch (_) {}
        return info;
    }

    function resolve(opts) {
        const o = opts || {};
        const loc = wl.locate(o);
        const lists = typeof loc.species === 'string' ? [loc.species] : Array.isArray(loc.species) ? loc.species : [];
        // A list file not there leaves its rows out (locate() names it missing).
        const present = lists.filter((l) => typeof l !== 'string' || exists(l));
        const species = wl.readSpecies(present);
        const config = {
            detector: modelInfo('owlv2', loc.detector),
            classifier: modelInfo(o.model || (o.classifier ? baseName(o.classifier) : DEFAULTS.model), loc.classifier),
            threshold: o.threshold == null ? DEFAULTS.threshold : o.threshold,
            queries: o.queries || QUERIES,
            topK: o.topK || DEFAULTS.topK,
            margin: o.margin == null ? DEFAULTS.margin : o.margin,
            // The boxes kept: suppress()'s { iou, apart, hold } (null: that rule off).
            nms: nmsConfig(o.nms, o.nmsApart, o.nmsHold),
            maxDetections: o.maxDetections || DEFAULTS.maxDetections,
            maxClassify: o.maxClassify || DEFAULTS.maxClassify,
            candidates: candidatesConfig(o.candidates, o.labelThreshold, o.petMinBoxScore),
            // Species a crop's suggestions come from: its top N over its query's classes.
            queryTop: o.queryTop == null ? DEFAULTS.queryTop : o.queryTop,
            // The list files by name, the merged count, and a hash of every
            // prompt (so an edited list is a new config). The taxa maps are
            // not in it: they name labels, records do not hold them.
            speciesList: { files: lists.filter((l) => typeof l === 'string').map(listName),
                           count: species.length, hash: fnv1a(species.map(speciesPrompt).join('\n')) },
        };
        return { loc, species, taxa: wl.readTaxa(present), config };
    }

    // config.nms: { iou, apart, hold }; nmsApart / nmsHold false is that rule off (null).
    function nmsConfig(iou, apart, hold) {
        const i = iou == null ? DEFAULTS.nms : iou;
        if (!(typeof i === 'number' && i >= 0)) throw new Error('bro.vision.wildlife: nms is a number >= 0, not ' + iou);
        const rule = (v, name) => {
            const x = v === false ? null : v == null ? DEFAULTS[name] : v;
            if (x !== null && !(typeof x === 'number' && x > 0 && x <= 1)) {
                throw new Error('bro.vision.wildlife: ' + name + ' is a number in (0, 1] or false, not ' + v);
            }
            return x;
        };
        return { iou: i, apart: rule(apart, 'nmsApart'), hold: rule(hold, 'nmsHold') };
    }

    // config.candidates: { mode: 'all', petMinBoxScore, petFallback } or
    // { mode: 'byQuery', fallback, petMinBoxScore, petFallback };
    // petMinBoxScore and petFallback null when the pet rule is off
    // (petMinBoxScore: false). petFallback is labelThreshold: a sure pet
    // box's crop is ranked over every species when that ranking gives
    // another class this much (classifyCrop's 'pet').
    function candidatesConfig(mode, labelThreshold, petMinBoxScore) {
        const m = mode || DEFAULTS.candidates;
        if (m !== 'all' && m !== 'byQuery') throw new Error("bro.vision.wildlife: candidates is 'all' or 'byQuery', not " + m);
        const pet = petMinBoxScore === false ? null
            : petMinBoxScore == null ? DEFAULTS.petMinBoxScore : petMinBoxScore;
        if (pet !== null && !(typeof pet === 'number' && pet >= 0)) {
            throw new Error('bro.vision.wildlife: petMinBoxScore is a number >= 0 or false, not ' + petMinBoxScore);
        }
        const t = labelThreshold == null ? DEFAULTS.labelThreshold : labelThreshold;
        const petFallback = pet === null ? null : t;
        return m === 'all' ? { mode: m, petMinBoxScore: pet, petFallback }
            : { mode: m, fallback: t, petMinBoxScore: pet, petFallback };
    }

    /** The settings an analysis depends on (open()'s `config`), without loading. */
    wl.config = function config(opts) { return resolve(opts).config; };

    // ── pixels ───────────────────────────────────────────────────────────

    function rgba(img) {
        let src = img.data || img.pixels;
        if ((img.channels || 4) === 3) {
            const out = new Uint8Array(img.width * img.height * 4);
            globalThis.bro.image.rgbToRgba(out, src, img.width * img.height, 255);
            src = out;
        }
        return src;
    }

    /**
     * Square around `box` ([x, y, w, h]) grown by `margin` on every side,
     * each side clamped to the w x h frame (so a square taller than the
     * frame comes back w x h-clipped, not square), or null for an empty box.
     */
    wl.subjectSquare = function subjectSquare(box, w, h, opts) {
        const margin = opts && opts.margin != null ? opts.margin : DEFAULTS.margin;
        const [bx, by, bw, bh] = box;
        if (!(bw > 0 && bh > 0)) return null;
        const side = Math.min(Math.max(w, h), Math.round(Math.max(bw, bh) * (1 + 2 * margin)));
        const sw = Math.min(side, w), sh = Math.min(side, h);
        const cx = bx + bw / 2, cy = by + bh / 2;
        const x = Math.round(Math.min(Math.max(cx - sw / 2, 0), w - sw));
        const y = Math.round(Math.min(Math.max(cy - sh / 2, 0), h - sh));
        return { x, y, w: sw, h: sh };
    };

    /** RGBA crop { width, height, data } of `rect` from an RGB or RGBA image. */
    wl.cropImage = function cropImage(img, rect) {
        const out = new Uint8Array(rect.w * rect.h * 4);
        globalThis.bro.image.cropU8(out, rgba(img), { srcW: img.width, srcH: img.height, channels: 4,
                                                     x: rect.x, y: rect.y, w: rect.w, h: rect.h });
        return { width: rect.w, height: rect.h, data: out };
    };

    /** The classifier's square RGBA input: centre crop, area resize (open_clip's framing). */
    wl.squareInput = function squareInput(img, size) {
        size = size || IMAGE_SIZE;
        const w = img.width, h = img.height, im = globalThis.bro.image;
        const src = rgba(img);
        const side = Math.min(w, h);
        let sq = src;
        if (w !== h) {
            sq = new Uint8Array(side * side * 4);
            im.centerCropU8(sq, src, { srcW: w, srcH: h, channels: 4, cropW: side, cropH: side });
        }
        if (side === size) return { width: size, height: size, data: sq };
        const out = new Uint8Array(size * size * 4);
        im.resizeU8(out, sq, { srcW: side, srcH: side, dstW: size, dstH: size, channels: 4,
                               filter: side > size ? 'area' : 'bicubic' });
        return { width: size, height: size, data: out };
    };

    /**
     * bro.image.sharpness of a whole image or a region, rounded to 2 places.
     * Region: `rect` in the image's pixels, or `box` ([x, y, w, h]) in the
     * pixels of `source` ({ width, height }, the photo the image was scaled from).
     */
    wl.sharpness = function sharpness(img, opts) {
        const o = opts || {};
        let rect = o.rect;
        if (!rect && o.box) {
            const sx = img.width / o.source.width, sy = img.height / o.source.height;
            const [x, y, w, h] = o.box;
            rect = { x: Math.max(0, Math.round(x * sx)), y: Math.max(0, Math.round(y * sy)),
                     w: Math.round(w * sx), h: Math.round(h * sy) };
        }
        const r = globalThis.bro.image.sharpness(img.pixels || img.data, {
            width: img.width, height: img.height, channels: img.channels || 4, rect });
        const r2 = (x) => Math.round(x * 100) / 100;
        return { laplacianVar: r2(r.laplacianVar), tenengrad: r2(r.tenengrad),
                 lumaMean: r2(r.lumaMean), lumaStd: r2(r.lumaStd), pixels: r.pixels };
    };

    // ── the analyzer ─────────────────────────────────────────────────────

    // The species text embeddings: from `cacheDir` when an earlier run wrote
    // them for this model and prompt list, else encoded (32 a batch) and saved.
    function textEmbeddings(clip, dir, prompts, o) {
        const fs = fsMod();
        const dim = clip.projectionDim;
        let size = 0;
        try { size = fs.statSync(dir + '/model.safetensors').size; } catch (_) {}
        const key = baseName(dir) + '-' + size + '-' +
            fnv1a(JSON.stringify(o.clipConfig || null) + '\n' + prompts.join('\n')) + '.f32';
        const text = new Float32Array(prompts.length * dim);
        if (o.cacheDir) {
            try {
                const buf = fs.readFileSync(o.cacheDir + '/' + key);
                if (buf && buf.byteLength === text.byteLength) {
                    const bytes = buf instanceof ArrayBuffer ? new Uint8Array(buf)
                        : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
                    new Uint8Array(text.buffer).set(bytes);
                    return { text, cached: true };
                }
            } catch (_) {}
        }
        const batch = 32;
        for (let i = 0; i < prompts.length; i += batch) {
            const embs = clip.encodeText(prompts.slice(i, i + batch));
            embs.forEach((e, j) => text.set(e, (i + j) * dim));
            if (o.onProgress) o.onProgress(Math.min(i + batch, prompts.length), prompts.length);
        }
        if (o.cacheDir) {
            try {
                fs.mkdirSync(o.cacheDir, { recursive: true });
                fs.writeFileSync(o.cacheDir + '/' + key, new Uint8Array(text.buffer));
            } catch (_) {}
        }
        return { text, cached: false };
    }

    /** Load the detector and classifier and embed the species list. */
    wl.open = function open(opts) {
        const o = opts || {};
        const bro = globalThis.bro;
        if (!bro.vision || typeof bro.vision.loadOwlv2 !== 'function' ||
            !bro.lm || typeof bro.lm.loadClip !== 'function') {
            throw new Error('bro.vision.wildlife.open: this build has no bro.vision.loadOwlv2 / bro.lm.loadClip');
        }
        const { loc, species, taxa, config } = resolve(o);
        if (loc.missing.length) {
            const where = (v) => (Array.isArray(v)
                ? v.map((x) => (typeof x === 'string' ? x : x === null ? 'not found' : 'rows')).join(' + ')
                : v || 'not found');
            const e = new Error('bro.vision.wildlife.open: not available, missing ' + loc.missing.map((k) =>
                k + ' (' + where(loc[k]) + ')').join(', '));
            e.code = 'missing';
            e.missing = loc.missing;
            throw e;
        }
        if (!species.length) throw new Error('bro.vision.wildlife.open: empty species list');
        const devOpts = o.device ? { device: o.device } : undefined;
        const owl = bro.vision.loadOwlv2(loc.detector, devOpts);
        const dir = slashes(loc.classifier);
        let clip, imageSize;
        try {
            const cfg = JSON.parse(String(fsMod().readFileSync(dir + '/config.json', 'utf-8')));
            const clipOpts = { vocabPath: dir + '/vocab.json', mergesPath: dir + '/merges.txt',
                               weightsPath: dir + '/model.safetensors' };
            if (o.device) clipOpts.device = o.device;
            // Architecture overrides over config.json (loadClip's `config`).
            if (o.clipConfig) clipOpts.config = o.clipConfig;
            clip = bro.lm.loadClip(clipOpts);
            imageSize = (cfg.vision_config && cfg.vision_config.image_size) || IMAGE_SIZE;
        } catch (e) { owl.dispose(); throw e; }
        const dim = clip.projectionDim;
        const prompts = species.map(speciesPrompt);
        // The app's cache dir by default; cacheDir: false never touches disk.
        const app = bro.app;
        const cacheDir = o.cacheDir === false ? null
            : o.cacheDir || (app && app.cacheDir ? app.cacheDir + '/wildlife' : null);
        const { text, cached } = textEmbeddings(clip, dir, prompts,
            { cacheDir, clipConfig: o.clipConfig, onProgress: o.onProgress });
        let disposed = false;
        const live = (what) => { if (disposed) throw new Error('bro.vision.wildlife: ' + what + ' after dispose()'); };

        // Cosine similarity of each embedding against every species prompt,
        // a Float32Array a species each, in one native pass over the prompts
        // (__bro_native.vision.dotRows): each a float64 sum from the first
        // dimension up, stored as a float, the numbers a JS loop
        // `d += emb[i] * text[off + i]` gives, ~30 times faster.
        function similarities(embList) {
            const n = species.length;
            const q = new Float32Array(embList.length * dim);
            for (let k = 0; k < embList.length; k++) q.set(embList[k], k * dim);
            const sims = __bro_native.vision.dotRows(q, text, dim);
            return embList.map((_, k) => sims.subarray(k * n, (k + 1) * n));
        }
        // The softmax at the model's temperature: `score` is a probability
        // over the candidates (how much its best answer stands out), not "is
        // this any listed species".
        const crop = (sims, c) => wl.classifyCrop(species, sims, c);

        const an = {
            config, species, taxa, prompts, cached, dim, imageSize,
            paths: { detector: loc.detector, classifier: loc.classifier,
                     species: (typeof loc.species === 'string' ? [loc.species] : loc.species)
                         .filter((l) => typeof l === 'string') },
            labelThreshold: o.labelThreshold == null ? DEFAULTS.labelThreshold : o.labelThreshold,
            // open()'s box rule options (labels()), the defaults where not given.
            boxRule: LABEL_BOX_KEYS.reduce((m, k) => { m[k] = o[k] == null ? DEFAULTS[k] : o[k]; return m; }, {}),
            get device() { return owl.device; },
            /** label() at this analyzer's labelThreshold and box rule, with its taxa names. */
            label(rec, burst) { return wl.labels(rec, burst, an.labelOptions())[0] || null; },
            /** labels() at this analyzer's labelThreshold and box rule, with its taxa names. */
            labels(rec, burst) { return wl.labels(rec, burst, an.labelOptions()); },
            /** The options labels() takes for this analyzer: { labelThreshold, taxa, ...boxRule }. */
            labelOptions() { return Object.assign({ labelThreshold: an.labelThreshold, taxa }, an.boxRule); },
            /**
             * OWLv2 boxes in `img`'s pixels, best first: [{ box, score, label }],
             * kept by suppress() (dopts nms, nmsApart, nmsHold over config's).
             */
            detect(img, dopts) {
                live('detect');
                const d = dopts || {};
                const pick = (v, c) => (v === undefined ? c : v === false ? null : v);
                const iou = d.nms == null ? config.nms.iou : d.nms;
                const apart = pick(d.nmsApart, config.nms.apart), hold = pick(d.nmsHold, config.nms.hold);
                const max = d.maxDetections || config.maxDetections;
                // Plain NMS runs in OWLv2; the other rules over every box at the threshold here.
                const plain = apart == null && hold == null;
                const hits = owl.detect(img, {
                    queries: d.queries || config.queries,
                    threshold: d.threshold == null ? config.threshold : d.threshold,
                    nms: plain ? iou : 0,
                    maxDetections: plain ? max : 3600,
                }).map((r) => ({ box: r.box, score: r.score, label: r.query }));
                return plain ? hits : wl.suppress(hits, { iou, apart, hold, max });
            },
            /** Species top K of a whole image (centre square), unrounded. */
            classify(img, copts) {
                live('classify');
                return crop(similarities([clip.encodeImage(wl.squareInput(img, imageSize))])[0],
                            { topK: (copts && copts.topK) || config.topK, candidates: 'all' }).species;
            },
            /**
             * The record of one photo: detections and subject in `source`
             * pixels (boxes to 0.1 px, scores to 4 places), the species top K
             * of the subject square, and every detection up to maxClassify
             * classified (`animals`); no box over the threshold is animal: false.
             */
            analyze(img, aopts) {
                live('analyze');
                const a = aopts || {};
                const src = a.source || { width: img.width, height: img.height };
                const sx = src.width / img.width, sy = src.height / img.height;
                const out = { animal: false, detections: [], subject: null, species: [], animals: [],
                              animalsDropped: 0, timing: { detect: 0, classify: null } };
                let t = Date.now();
                const hits = an.detect(img, { threshold: a.threshold });
                out.timing.detect = Date.now() - t;
                out.animal = hits.length > 0;
                const srcBox = (b) => [r1(b[0] * sx), r1(b[1] * sy), r1(b[2] * sx), r1(b[3] * sy)];
                out.detections = hits.map((h) => ({ label: h.label, score: r4(h.score), box: srcBox(h.box) }));
                if (!out.animal) return out;
                t = Date.now();
                const maxClassify = a.maxClassify || config.maxClassify;
                const mode = a.candidates || a.petMinBoxScore != null
                    ? candidatesConfig(a.candidates || config.candidates.mode, an.labelThreshold,
                                       a.petMinBoxScore != null ? a.petMinBoxScore
                                           : config.candidates.petMinBoxScore === null ? false : config.candidates.petMinBoxScore)
                    : config.candidates;
                const copts = { topK: a.topK || config.topK };
                const roundSp = (s) => ({ common: s.common, scientific: s.scientific, family: s.family,
                    order: s.order, class: s.class, score: r4(s.score), similarity: r4(s.similarity) });
                const roundTaxa = (taxa) => {
                    const o2 = {};
                    for (const r of RANKS.slice(1)) o2[r] = taxa[r].map((e) => Object.assign({}, e, { score: r4(e.score) }));
                    return o2;
                };
                // Best first, up to maxClassify; an empty box is no animal.
                // Every crop goes through the vision tower in one batch
                // (encodeImages: element i is encodeImage of crop i).
                const picked = [];
                for (let i = 0; i < hits.length; i++) {
                    const sq = wl.subjectSquare(hits[i].box, img.width, img.height, { margin: config.margin });
                    if (!sq) continue;
                    if (picked.length >= maxClassify) { out.animalsDropped++; continue; }
                    picked.push({ i, sq });
                }
                const embs = picked.length
                    ? clip.encodeImages(picked.map((p) => wl.squareInput(wl.cropImage(img, p.sq), imageSize)))
                    : [];
                const sims = embs.length ? similarities(embs) : [];
                for (let k = 0; k < picked.length; k++) {
                    const { i, sq } = picked[k];
                    const h = hits[i];
                    const subject = { x: Math.round(sq.x * sx), y: Math.round(sq.y * sy),
                                      w: Math.round(sq.w * sx), h: Math.round(sq.h * sy) };
                    const cand = wl.candidatesFor(h.label, out.detections[i].score, mode);
                    const c = crop(sims[k], Object.assign({}, copts, { query: h.label, candidates: cand,
                        fallback: cand === 'pet' ? mode.petFallback : mode.fallback }));
                    const animal = { box: out.detections[i].box, score: out.detections[i].score, label: h.label,
                                     subject, species: c.species.map(roundSp), taxa: roundTaxa(c.taxa),
                                     candidates: c.candidates };
                    // The crop's image embedding (unit length) on request,
                    // for "looks like the one you named": not record data,
                    // so a caller keeps it out of what it stores as JSON.
                    if (a.embeddings) animal.embedding = Float32Array.from(embs[k]);
                    // What to suggest for the crop: its top over its query's
                    // classes, whatever it was ranked over (labels() never reads it).
                    if (config.queryTop > 0) {
                        animal.queryTop = wl.queryTop(species, sims[k], h.label, config.queryTop)
                            .map((s) => ({ common: s.common, scientific: s.scientific, score: r4(s.score) }));
                    }
                    // A pet box kept to its query's classes: the answer over
                    // every species too, its top K and each rank's best.
                    if (c.unrestricted) {
                        const u = c.unrestricted, best = {};
                        for (const r of RANKS.slice(1)) best[r] = u.taxa[r].slice(0, 1);
                        animal.unrestricted = { species: u.species.map(roundSp), taxa: roundTaxa(best) };
                    }
                    out.animals.push(animal);
                }
                // The best box's, as before animals: subject and species.
                if (out.animals.length) {
                    out.subject = out.animals[0].subject;
                    out.species = out.animals[0].species;
                }
                out.timing.classify = Date.now() - t;
                return out;
            },
            dispose() {
                if (disposed) return;
                disposed = true;
                owl.dispose();
                if (clip && typeof clip.dispose === 'function') clip.dispose();
            },
        };
        return an;
    };

    // ── bursts ───────────────────────────────────────────────────────────

    const TIME_RE = /^(\d{4})-(\d\d)-(\d\d)[T ](\d\d):(\d\d):(\d\d)(?:\.(\d+))?(Z|[+-]\d\d:?\d\d)?$/;

    /**
     * A readExif dateTaken ("2026-10-05T11:34:38.56-05:00") as milliseconds;
     * without an offset the clock time is read as UTC. Null when it does not parse.
     */
    wl.parseExifTime = function parseExifTime(s) {
        const m = typeof s === 'string' && TIME_RE.exec(s.trim());
        if (!m) return null;
        const frac = m[7] ? Number('0.' + m[7]) * 1000 : 0;
        let t = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) + frac;
        if (m[8] && m[8] !== 'Z') {
            const sign = m[8][0] === '-' ? -1 : 1;
            const hh = +m[8].slice(1, 3), mm = +m[8].slice(-2);
            t -= sign * (hh * 60 + mm) * 60000;
        }
        return Number.isFinite(t) ? Math.round(t * 1000) / 1000 : null;
    };

    // One frame's vote: each of its top-K species adds its score.
    function vote(tally, rec) {
        for (const s of rec.species || []) {
            const k = s.scientific || s.common;
            const t = tally.get(k) || { common: s.common, scientific: s.scientific, sum: 0, top1: 0 };
            t.sum += s.score;
            tally.set(k, t);
        }
        const top = rec.species && rec.species[0];
        if (top) tally.get(top.scientific || top.common).top1++;
    }

    /** The sharpness a keeper is chosen by: the subject's when there is an animal. */
    wl.keeperScore = function keeperScore(rec, metric) {
        const sh = rec.sharpness;
        if (!sh) return -1;
        const m = metric || 'tenengrad';
        const pick = (o) => (o ? o[m] : -1);
        return rec.animal && sh.subject ? pick(sh.subject) : pick(sh.frame);
    };

    const round = (x, d) => {
        const k = Math.pow(10, d == null ? 3 : d);
        return x == null ? null : Math.round(x * k) / k;
    };

    function summarize(g, id, metric, pt) {
        const shots = g.shots;
        const animals = shots.filter((r) => r.animal);
        // The classes the burst's crops are sure of (pet crops kept to their
        // query's classes left out), which its pet crops are weighed against.
        const classes = sureClasses(animals.reduce((all, r) => all.concat(cropsOf(r)), []), pt.t, pt.broadMin);
        const pets = { t: pt.t, classes };
        const tally = new Map();
        for (const r of animals) vote(tally, r);
        let best = null;
        for (const t of tally.values()) if (!best || t.sum > best.sum) best = t;
        let keeper = shots[0], keeperScoreV = -Infinity;
        for (const r of animals.length ? animals : shots) {
            const s = wl.keeperScore(r, metric);
            if (s > keeperScoreV) { keeper = r; keeperScoreV = s; }
        }
        const first = shots[0], last = shots[shots.length - 1];
        // Per taxon at every rank: each animal frame's score (the best of its
        // crops), and the share (their sum over the animal frames). labels()
        // applies the threshold.
        // `evidence` per frame: its crops' [score, boxScore, flags] (addEvidence),
        // which labels() weighs with its box rule.
        const taxa = new Map();
        for (const r of animals) {
            const frame = new Map();
            for (const c of cropsOf(r, pets)) {
                const box = c.score == null ? 1 : round(c.score, 4);
                for (const e of cropEntries(c)) {
                    const k = e.rank + '|' + e.scientific;
                    let f = frame.get(k);
                    if (!f) { f = { e, ev: [] }; frame.set(k, f); }
                    else if (e.score > f.e.score) f.e = e;
                    const flags = (c.primary ? EV_PRIMARY : 0) | (queryMatches(c.label, e.rank, e) ? EV_MATCH : 0);
                    addEvidence(f.ev, round(e.score, 4), box, flags);
                }
            }
            for (const [k, { e, ev }] of frame) {
                let T = taxa.get(k);
                if (!T) {
                    T = Object.assign({ rank: e.rank, scientific: e.scientific }, e.common ? { common: e.common } : {},
                                      e.lineage, { share: 0, scores: [], evidence: [] });
                    taxa.set(k, T);
                }
                T.scores.push(e.score);
                T.evidence.push(ev);
            }
        }
        const taxaOut = Array.from(taxa.values()).map((T) => {
            T.scores.sort((a, b) => b - a);
            T.share = round(T.scores.reduce((s, v) => s + v, 0) / animals.length, 4);
            T.scores = T.scores.map((v) => round(v, 4));
            return T;
        }).sort((a, b) => b.share - a.share || rankIx(a.rank) - rankIx(b.rank) ||
            (a.scientific < b.scientific ? -1 : 1));
        return {
            id,
            first: first.path,
            start: first.exif && first.exif.dateTaken || null,
            durationS: first.time != null && last.time != null ? round((last.time - first.time) / 1000) : 0,
            count: shots.length,
            animalCount: animals.length,
            species: best ? {
                common: best.common, scientific: best.scientific,
                share: round(best.sum / animals.length, 4),
                agree: best.top1,
            } : null,
            taxa: taxaOut,
            sureClasses: classes,
            keeper: keeper.path,
            keeperSharpness: round(keeperScoreV, 2),
            startedBy: g.splitBy || 'gap',
            maxGapS: g.gaps.length ? round(Math.max(...g.gaps) / 1000) : null,
            minNcc: g.ncc.some((v) => v != null) ? round(Math.min(...g.ncc.filter((v) => v != null)), 4) : null,
            photos: shots.map((r) => r.path),
        };
    }

    /**
     * Group photo records into bursts: by capture time, a shot joins the one
     * before it when at most `gapS` seconds later (and `sameScene`, if given,
     * does not split them). Records with an `error` are left out. A burst's
     * `sureClasses` are the classes its crops (pet crops kept to their
     * query's classes left out) are sure of at `labelThreshold` on boxes at
     * `broadMinBoxScore` (opts, DEFAULTS'), and its pet crops count in its
     * `taxa` by petAnswer against them.
     */
    wl.groupBursts = function groupBursts(records, opts) {
        const o = opts || {};
        const gapMs = (o.gapS == null ? DEFAULTS.gapS : o.gapS) * 1000;
        const shots = records.filter((r) => !r.error).slice().sort((a, b) =>
            (a.time == null) - (b.time == null) || (a.time || 0) - (b.time || 0) ||
            (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
        const groups = [];
        let cur = null;
        for (const r of shots) {
            const prev = cur && cur.shots[cur.shots.length - 1];
            const near = !!prev && prev.time != null && r.time != null && r.time - prev.time <= gapMs;
            let join = near, scene = null;
            if (join && o.sameScene) {
                scene = o.sameScene(prev, r);
                if (scene && scene.split) join = false;
            }
            if (join) {
                cur.shots.push(r);
                cur.gaps.push(r.time - prev.time);
                cur.ncc.push(scene ? scene.ncc : null);
            } else {
                cur = { shots: [r], gaps: [], ncc: [], splitBy: near ? 'scene' : null };
                groups.push(cur);
            }
        }
        const pt = { t: o.labelThreshold == null ? DEFAULTS.labelThreshold : o.labelThreshold,
                     broadMin: o.broadMinBoxScore == null ? DEFAULTS.broadMinBoxScore : o.broadMinBoxScore };
        return groups.map((g, i) => summarize(g, i + 1, o.metric, pt));
    };

    globalThis.__bro_vision_wildlife = wl;
})();
