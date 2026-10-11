// bro.vision.wildlife — what is in a wildlife photo, and which shot of a burst
// to keep (docs/vision-api.js, "Wildlife"). Pure JS over bro.vision.loadOwlv2,
// bro.lm.loadClip and bro.image: OWLv2 finds the animal from text queries, the
// best box padded to a square is BioCLIP's input, and BioCLIP names the
// species zero-shot over a species list (one prompt per species, embedded
// once). Sharpness and burst grouping pick the keeper of a run of shots.
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
// Kept bit-identical to broworkshop's ai/birds pipeline, whose records
// (RECORDS.md there) are the reference: the queries, threshold, NMS, square
// margin, prompt recipe, temperature, ranking and rounding are its.
(function () {
    'use strict';

    const wl = {};

    /** OWLv2 queries: the animals of a yard species list. */
    const QUERIES = [
        'a bird', 'a squirrel', 'a chipmunk', 'a rabbit', 'a deer', 'a raccoon', 'a fox',
        'a woodchuck', 'a skunk', 'a beaver', 'a muskrat', 'an otter', 'a mink',
    ];
    // Clear animals score 0.3 and up; real ones half hidden and look-alikes
    // share 0.1-0.3; empty frames score under 0.1.
    const DEFAULTS = { model: 'bioclip-2', threshold: 0.15, topK: 5, margin: 0.15,
                       nms: 0.3, maxDetections: 20, gapS: 1.0 };
    const MODEL_REPO = { 'bioclip': 'imageomics/bioclip', 'bioclip-2': 'imageomics/bioclip-2',
                         'owlv2': 'google/owlv2-base-patch16-ensemble' };
    const SPECIES_LIST = 'upper-midwest.json';
    const IMAGE_SIZE = 224;
    // open_clip's trained temperature: exp(logit_scale) = 100 for both BioCLIPs.
    const LOGIT_SCALE = 100;

    wl.QUERIES = QUERIES;
    wl.DEFAULTS = DEFAULTS;

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
     * bro.models, so every path must be given there.
     */
    wl.locate = function locate(opts) {
        const o = opts || {};
        const models = globalThis.bro && globalThis.bro.models;
        const find = models && typeof models.find === 'function' ? (n) => models.find(n) : null;
        const model = o.model || DEFAULTS.model;
        const out = { detector: o.detector || null, classifier: o.classifier || null,
                      species: o.species || null, model, missing: [] };
        if (!find && (!o.detector || !o.classifier || !o.species)) {
            throw new Error('bro.vision.wildlife: bro.models is main-realm only; in a Worker pass ' +
                            'detector, classifier and species (paths)');
        }
        if (!out.detector) out.detector = find('owlv2');
        if (!out.classifier) out.classifier = find(model);
        if (!out.species) {
            const dir = find('species');
            out.species = dir ? dir + '/' + SPECIES_LIST : null;
        }
        if (!out.detector || !exists(out.detector + '/model.safetensors')) out.missing.push('detector');
        if (!out.classifier || !exists(out.classifier + '/model.safetensors') ||
            !exists(out.classifier + '/config.json')) out.missing.push('classifier');
        if (typeof out.species === 'string' && !exists(out.species)) out.missing.push('species');
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

    /** The rows of a species list file ({ species: [...] } or an array). */
    wl.readSpeciesList = function readSpeciesList(path) {
        const doc = JSON.parse(String(fsMod().readFileSync(path, 'utf-8')));
        return doc.species || doc;
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
        const species = Array.isArray(loc.species) ? loc.species
            : (loc.species && exists(loc.species) ? wl.readSpeciesList(loc.species) : []);
        const config = {
            detector: modelInfo('owlv2', loc.detector),
            classifier: modelInfo(o.model || (o.classifier ? baseName(o.classifier) : DEFAULTS.model), loc.classifier),
            threshold: o.threshold == null ? DEFAULTS.threshold : o.threshold,
            queries: o.queries || QUERIES,
            topK: o.topK || DEFAULTS.topK,
            margin: o.margin == null ? DEFAULTS.margin : o.margin,
            speciesList: { file: typeof loc.species === 'string' ? baseName(loc.species) : null,
                           count: species.length },
        };
        return { loc, species, config };
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
        const { loc, species, config } = resolve(o);
        if (loc.missing.length) {
            const e = new Error('bro.vision.wildlife.open: not available, missing ' + loc.missing.map((k) =>
                k + ' (' + (Array.isArray(loc[k]) ? 'rows' : loc[k] || 'not found') + ')').join(', '));
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

        // Cosine similarity against every species, then the softmax at the
        // model's temperature: `score` is a probability over the list (how
        // much its best answer stands out), not "is this any listed species".
        function rank(emb, topK) {
            const n = species.length;
            const sims = new Float32Array(n);
            let max = -Infinity;
            for (let s = 0; s < n; s++) {
                let d = 0;
                const off = s * dim;
                for (let i = 0; i < dim; i++) d += emb[i] * text[off + i];
                sims[s] = d;
                if (d > max) max = d;
            }
            let z = 0;
            const probs = new Float32Array(n);
            for (let s = 0; s < n; s++) { probs[s] = Math.exp((sims[s] - max) * LOGIT_SCALE); z += probs[s]; }
            const order = Array.from({ length: n }, (_, i) => i).sort((a, b) => sims[b] - sims[a]);
            return order.slice(0, topK).map((s) => {
                const sp = species[s];
                return { common: sp.common, scientific: sp.scientific, order: sp.order,
                         family: sp.family, class: sp.class, score: probs[s] / z, similarity: sims[s] };
            });
        }

        const an = {
            config, species, prompts, cached, dim, imageSize,
            paths: { detector: loc.detector, classifier: loc.classifier,
                     species: typeof loc.species === 'string' ? loc.species : null },
            get device() { return owl.device; },
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
                return rank(clip.encodeImage(wl.squareInput(img, imageSize)),
                            (copts && copts.topK) || config.topK);
            },
            /**
             * The record of one photo: detections and subject in `source`
             * pixels (boxes to 0.1 px, scores to 4 places), the species top K
             * of the subject square; no box over the threshold is animal: false.
             */
            analyze(img, aopts) {
                const a = aopts || {};
                const src = a.source || { width: img.width, height: img.height };
                const sx = src.width / img.width, sy = src.height / img.height;
                const out = { animal: false, detections: [], subject: null, species: [],
                              timing: { detect: 0, classify: null } };
                let t = Date.now();
                const hits = an.detect(img, { threshold: a.threshold });
                out.timing.detect = Date.now() - t;
                out.animal = hits.length > 0;
                out.detections = hits.map((h) => ({ label: h.label, score: r4(h.score),
                    box: [r1(h.box[0] * sx), r1(h.box[1] * sy), r1(h.box[2] * sx), r1(h.box[3] * sy)] }));
                if (!out.animal) return out;
                t = Date.now();
                const sq = wl.subjectSquare(hits[0].box, img.width, img.height, { margin: config.margin });
                out.subject = { x: Math.round(sq.x * sx), y: Math.round(sq.y * sy),
                                w: Math.round(sq.w * sx), h: Math.round(sq.h * sy) };
                out.species = an.classify(wl.cropImage(img, sq), { topK: a.topK }).map((s) => ({
                    common: s.common, scientific: s.scientific, family: s.family, order: s.order,
                    class: s.class, score: r4(s.score), similarity: r4(s.similarity) }));
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
