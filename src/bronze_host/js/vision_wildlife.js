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
    // Clear animals score 0.3 and up; real ones half hidden and look-alikes
    // share 0.1-0.3; empty frames score under 0.1. A score under
    // labelThreshold is not shown as a name (labels()).
    const DEFAULTS = { model: 'bioclip-2', threshold: 0.15, topK: 5, margin: 0.15,
                       nms: 0.3, maxDetections: 20, gapS: 1.0, labelThreshold: 0.85,
                       maxClassify: 8, candidates: 'all', taxaTop: 3 };
    const MODEL_REPO = { 'bioclip': 'imageomics/bioclip', 'bioclip-2': 'imageomics/bioclip-2',
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
    // The ranks a label can be at, narrowest first.
    const RANKS = ['species', 'genus', 'family', 'order', 'class'];
    const IMAGE_SIZE = 224;
    // open_clip's trained temperature: exp(logit_scale) = 100 for both BioCLIPs.
    const LOGIT_SCALE = 100;

    wl.QUERIES = QUERIES;
    wl.QUERY_CLASSES = QUERY_CLASSES;
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
        const maps = { genus: new Map(), family: new Map(), order: new Map(), class: new Map() };
        for (let i = 0; i < rows.length; i++) {
            const row = rows[i], p = probs[i];
            if (!row || !(p > 0)) continue;
            const lin = lineageOf(row, 'species');
            for (let ri = 1; ri < RANKS.length; ri++) {
                const r = RANKS[ri], name = lin[r];
                if (!name) continue;
                let t = maps[r].get(name);
                if (!t) {
                    t = Object.assign({ scientific: name, score: 0 }, lineageOf(lin, r));
                    maps[r].set(name, t);
                }
                t.score += p;
            }
        }
        const out = {};
        for (const r of RANKS.slice(1)) {
            const list = Array.from(maps[r].values()).sort((a, b) => b.score - a.score ||
                (a.scientific < b.scientific ? -1 : 1));
            out[r] = top ? list.slice(0, top) : list;
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
     * @param {Object[]} rows   the species rows
     * @param {ArrayLike<number>} sims  cosine similarity per row
     * @param {{ topK?, top?, query?, candidates?: 'all'|'byQuery', fallback? }} [opts]
     * @returns {{ species: Array<{ common, scientific, family, order, class,
     *   score, similarity }>, taxa: (taxaScores), candidates: 'all'|'query'|'fallback' }}
     *   unrounded
     */
    wl.classifyCrop = function classifyCrop(rows, sims, opts) {
        const o = opts || {};
        const topK = o.topK || DEFAULTS.topK;
        const fallback = o.fallback != null ? o.fallback : DEFAULTS.labelThreshold;
        const all = Array.from({ length: rows.length }, (_, i) => i);
        const rankOver = (idx) => {
            const p = softmaxOver(sims, idx);
            const order = Array.from({ length: idx.length }, (_, k) => k)
                .sort((a, b) => sims[idx[b]] - sims[idx[a]] || a - b);
            const species = order.slice(0, topK).map((k) => {
                const sp = rows[idx[k]];
                return { common: sp.common, scientific: sp.scientific, order: sp.order,
                         family: sp.family, class: sp.class, score: p[k], similarity: sims[idx[k]] };
            });
            return { species, taxa: wl.taxaScores(idx.map((i) => rows[i]), p, { top: o.top }) };
        };
        const classes = (o.candidates || DEFAULTS.candidates) === 'byQuery' ? wl.queryClasses(o.query) : null;
        if (classes) {
            const sub = all.filter((i) => classes.includes(rows[i].class));
            if (sub.length && sub.length < all.length) {
                const q = rankOver(sub);
                if (q.species[0].score >= fallback) return Object.assign(q, { candidates: 'query' });
                const a = rankOver(all);
                if (a.species[0].score >= fallback && !classes.includes(a.species[0].class)) {
                    return Object.assign(a, { candidates: 'fallback' });
                }
                return Object.assign(q, { candidates: 'query' });
            }
        }
        return Object.assign(rankOver(all), { candidates: 'all' });
    };

    // A record's crops: its `animals`, or for a record from before them, its
    // top K as the one crop (taxa summed over the top K), on the best box.
    function cropsOf(rec) {
        if (!rec || rec.error || !rec.animal) return [];
        if (Array.isArray(rec.animals)) return rec.animals.filter((a) => a && a.species && a.species.length);
        if (!rec.species || !rec.species.length) return [];
        const d = rec.detections && rec.detections[0];
        return [{ box: d ? d.box : null, species: rec.species,
                  taxa: wl.taxaScores(rec.species, rec.species.map((s) => s.score), { top: 0 }) }];
    }

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
     * A crop's label: the narrowest rank whose best score is at least `t`,
     * or null (found, not identified).
     */
    function cropLabel(crop, t) {
        for (const r of RANKS) {
            const e = cropTop(crop, r);
            if (e && e.score >= t) {
                return { rank: r, scientific: e.scientific, common: r === 'species' ? e.common : undefined,
                         score: e.score, lineage: lineageOf(e, r) };
            }
        }
        return null;
    }

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

    function photoLabels(rec, t) {
        const groups = new Map();
        for (const crop of cropsOf(rec)) {
            const l = cropLabel(crop, t);
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

    function burstLabels(burst, t) {
        if (!Array.isArray(burst.taxa)) {
            // A burst from before per-taxon consensus: its one species.
            const s = burst.species;
            return s && s.share >= t ? [{ rank: 'species', scientific: s.scientific, common: s.common,
                score: s.share, share: s.share, frames: s.agree, lineage: {}, boxes: [] }] : [];
        }
        const out = [];
        for (const T of burst.taxa) {
            const frames = (T.scores || []).filter((s) => s >= t).length;
            if (!(T.share >= t) && !frames) continue;
            out.push({ rank: T.rank, scientific: T.scientific, common: T.common, share: T.share,
                       score: Math.max(T.share, (T.scores && T.scores[0]) || 0), frames,
                       lineage: lineageOf(T, T.rank), boxes: [] });
        }
        return dropBroader(out);
    }

    // A frame's boxes for its burst's labels: each crop goes to the
    // narrowest label that is its own best guess at that label's rank.
    function frameBoxes(labels, rec) {
        const ordered = labels.slice().sort((a, b) => rankIx(a.rank) - rankIx(b.rank));
        for (const crop of cropsOf(rec)) {
            if (!crop.box) continue;
            for (const L of ordered) {
                const e = cropTop(crop, L.rank);
                if (e && e.scientific === L.scientific) { addBoxes(L.boxes, [crop.box]); break; }
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
     * is the burst's own.
     * @param {Object|null} rec
     * @param {Object|null} [burst]
     * @param {{ labelThreshold?: number, taxa?: Object }} [opts]  `taxa`:
     *   readTaxa's map, the common names of genera and up
     * @returns {Array<{ common, scientific, rank, score, from, boxes,
     *                   frames?, share? }>}
     */
    wl.labels = function labels(rec, burst, opts) {
        const o = opts || {};
        const t = o.labelThreshold != null ? o.labelThreshold : DEFAULTS.labelThreshold;
        if (burst) {
            const bl = burstLabels(burst, t);
            if (bl.length) {
                if (rec) frameBoxes(bl, rec);
                return finish(bl, 'burst', o.taxa);
            }
        }
        return rec ? finish(photoLabels(rec, t), 'photo', o.taxa) : [];
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
            maxClassify: o.maxClassify || DEFAULTS.maxClassify,
            candidates: candidatesConfig(o.candidates, o.labelThreshold),
            // The list files by name, the merged count, and a hash of every
            // prompt (so an edited list is a new config). The taxa maps are
            // not in it: they name labels, records do not hold them.
            speciesList: { files: lists.filter((l) => typeof l === 'string').map(listName),
                           count: species.length, hash: fnv1a(species.map(speciesPrompt).join('\n')) },
        };
        return { loc, species, taxa: wl.readTaxa(present), config };
    }

    // config.candidates: { mode: 'all' } or { mode: 'byQuery', fallback }.
    function candidatesConfig(mode, labelThreshold) {
        const m = mode || DEFAULTS.candidates;
        if (m !== 'all' && m !== 'byQuery') throw new Error("bro.vision.wildlife: candidates is 'all' or 'byQuery', not " + m);
        return m === 'all' ? { mode: m }
            : { mode: m, fallback: labelThreshold == null ? DEFAULTS.labelThreshold : labelThreshold };
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

        // Cosine similarity against every species prompt.
        function similarities(emb) {
            const n = species.length;
            const sims = new Float32Array(n);
            for (let s = 0; s < n; s++) {
                let d = 0;
                const off = s * dim;
                for (let i = 0; i < dim; i++) d += emb[i] * text[off + i];
                sims[s] = d;
            }
            return sims;
        }
        // The softmax at the model's temperature: `score` is a probability
        // over the candidates (how much its best answer stands out), not "is
        // this any listed species".
        const crop = (emb, c) => wl.classifyCrop(species, similarities(emb), c);

        const an = {
            config, species, taxa, prompts, cached, dim, imageSize,
            paths: { detector: loc.detector, classifier: loc.classifier,
                     species: (typeof loc.species === 'string' ? [loc.species] : loc.species)
                         .filter((l) => typeof l === 'string') },
            labelThreshold: o.labelThreshold == null ? DEFAULTS.labelThreshold : o.labelThreshold,
            get device() { return owl.device; },
            /** label() at this analyzer's labelThreshold, with its taxa names. */
            label(rec, burst) { return wl.label(rec, burst, { labelThreshold: an.labelThreshold, taxa }); },
            /** labels() at this analyzer's labelThreshold, with its taxa names. */
            labels(rec, burst) { return wl.labels(rec, burst, { labelThreshold: an.labelThreshold, taxa }); },
            /** OWLv2 boxes in `img`'s pixels, best first: [{ box, score, label }]. */
            detect(img, dopts) {
                live('detect');
                const d = dopts || {};
                const hits = owl.detect(img, {
                    queries: d.queries || config.queries,
                    threshold: d.threshold == null ? config.threshold : d.threshold,
                    nms: d.nms == null ? DEFAULTS.nms : d.nms,
                    maxDetections: d.maxDetections || DEFAULTS.maxDetections,
                });
                return hits.map((r) => ({ box: r.box, score: r.score, label: r.query }));
            },
            /** Species top K of a whole image (centre square), unrounded. */
            classify(img, copts) {
                live('classify');
                return crop(clip.encodeImage(wl.squareInput(img, imageSize)),
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
                const mode = a.candidates ? candidatesConfig(a.candidates, an.labelThreshold) : config.candidates;
                const copts = { topK: a.topK || config.topK, candidates: mode.mode, fallback: mode.fallback };
                const roundSp = (s) => ({ common: s.common, scientific: s.scientific, family: s.family,
                    order: s.order, class: s.class, score: r4(s.score), similarity: r4(s.similarity) });
                const roundTaxa = (taxa) => {
                    const o2 = {};
                    for (const r of RANKS.slice(1)) o2[r] = taxa[r].map((e) => Object.assign({}, e, { score: r4(e.score) }));
                    return o2;
                };
                // Best first, up to maxClassify; an empty box is no animal.
                for (let i = 0; i < hits.length; i++) {
                    const h = hits[i];
                    const sq = wl.subjectSquare(h.box, img.width, img.height, { margin: config.margin });
                    if (!sq) continue;
                    if (out.animals.length >= maxClassify) { out.animalsDropped++; continue; }
                    const subject = { x: Math.round(sq.x * sx), y: Math.round(sq.y * sy),
                                      w: Math.round(sq.w * sx), h: Math.round(sq.h * sy) };
                    const emb = clip.encodeImage(wl.squareInput(wl.cropImage(img, sq), imageSize));
                    const c = crop(emb, Object.assign({ query: h.label }, copts));
                    out.animals.push({ box: out.detections[i].box, score: out.detections[i].score, label: h.label,
                                       subject, species: c.species.map(roundSp), taxa: roundTaxa(c.taxa),
                                       candidates: c.candidates });
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

    function summarize(g, id, metric) {
        const shots = g.shots;
        const animals = shots.filter((r) => r.animal);
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
        const taxa = new Map();
        for (const r of animals) {
            const frame = new Map();
            for (const c of cropsOf(r)) {
                for (const e of cropEntries(c)) {
                    const k = e.rank + '|' + e.scientific;
                    const f = frame.get(k);
                    if (!f || e.score > f.score) frame.set(k, e);
                }
            }
            for (const [k, e] of frame) {
                let T = taxa.get(k);
                if (!T) {
                    T = Object.assign({ rank: e.rank, scientific: e.scientific }, e.common ? { common: e.common } : {},
                                      e.lineage, { share: 0, scores: [] });
                    taxa.set(k, T);
                }
                T.scores.push(e.score);
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
     * does not split them). Records with an `error` are left out.
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
        return groups.map((g, i) => summarize(g, i + 1, o.metric));
    };

    globalThis.__bro_vision_wildlife = wl;
})();
