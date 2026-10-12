// The Worker half of test_vision_wildlife.js: bro.vision.wildlife in a
// worker realm, which has no bro.models, so locate() needs paths.
self.onmessage = (e) => {
    const m = e.data;
    const r = { ok: true };
    try {
        const wl = bro.vision && bro.vision.wildlife;
        r.surface = !!wl && typeof wl.open === 'function' && typeof wl.groupBursts === 'function';
        try { wl.locate(); r.noModels = 'no throw'; } catch (err) { r.noModels = String(err.message); }
        const none = { detector: m.empty, classifier: m.empty, species: m.empty + '/list.json' };
        r.missing = wl.locate(none).missing;
        r.available = wl.available(none);
        r.bursts = wl.groupBursts(m.records, { gapS: 1 });
        r.count = wl.config({ detector: m.empty, classifier: m.empty, species: m.list }).speciesList.count;
        r.merged = wl.config({ detector: m.empty, classifier: m.empty, species: [m.list, m.list2] }).speciesList.count;
        const lbl = (t) => { const l = wl.label(null, r.bursts[0], { labelThreshold: t }); return l && l.common; };
        r.labels = [lbl(0.65), lbl(0.85), lbl(0.95)];
        // Animals, ranks and per-taxon bursts.
        r.bBursts = wl.groupBursts(m.bRecs, { gapS: 1 });
        const opts = { taxa: m.taxa };
        r.bLabels = m.bRecs.map((rec) => wl.labels(rec, r.bBursts.find((b) => b.photos.includes(rec.path)), opts));
        r.bAlone = m.bRecs.map((rec) => wl.labels(rec, null, opts));
        r.crop = wl.classifyCrop(m.rows, m.cfSims, { candidates: 'byQuery', query: 'a moth' });
        // What open()'s similarities run on, registered on this thread too.
        r.dot = Array.from(__bro_native.vision.dotRows(new Float32Array([1, 2, 2, 4]), new Float32Array([3, 4, 5, 6]), 2));
    } catch (err) {
        r.ok = false;
        r.error = String((err && err.message) || err);
    }
    self.postMessage(r);
};
