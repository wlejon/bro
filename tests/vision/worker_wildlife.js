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
    } catch (err) {
        r.ok = false;
        r.error = String((err && err.message) || err);
    }
    self.postMessage(r);
};
