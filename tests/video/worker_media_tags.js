// Worker for test_media_tags.js: bro.media.tags on each path it is sent,
// answered as { name: tags | null }.
self.onmessage = (e) => {
    try {
        const out = {};
        for (const [name, p] of Object.entries(e.data)) out[name] = bro.media.tags(p);
        self.postMessage(out);
    } catch (err) {
        self.postMessage({ error: String(err && err.stack || err) });
    }
};
