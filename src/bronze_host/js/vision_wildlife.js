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
    const rankIx = (r) => RANKS.indexOf(r);
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
    wl._shared = { fsMod, slashes, baseName, exists, r1, r4, fnv1a, listName, normName, lineageOf, MODEL_REPO, LABEL_BOX_KEYS, IMAGE_SIZE };
    globalThis.__bro_vision_wildlife = wl;
})();
