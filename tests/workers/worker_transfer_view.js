// Worker side of test_postmessage_transfer_view: report what the views of a
// transferred buffer arrived as.
self.onmessage = (e) => {
    const d = e.data;
    self.postMessage({
        vals: Array.from(d.v),
        offset: d.v.byteOffset,
        bufLen: d.v.buffer.byteLength,
        rawLen: d.raw ? d.raw.byteLength : -1,
    });
};
