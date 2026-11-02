// bro.vision.wildlife — labels, analyzer (open/analyze), sharpness, EXIF time, and burst grouping
// (split from vision_wildlife.js).
(function () {
    'use strict';
    const wl = globalThis.__bro_vision_wildlife;
    const { QUERIES, QUERY_CLASSES, PET_QUERIES, DEFAULTS, RANKS, speciesPrompt } = wl;
    const { fsMod, slashes, baseName, exists, r1, r4, fnv1a, listName, normName, lineageOf, MODEL_REPO, LABEL_BOX_KEYS, IMAGE_SIZE } = wl._shared;
    delete wl._shared;

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

})();
