// Worker side of test_postmessage_buffer_identity: report whether the views
// that were posted over one buffer arrived over one buffer, then send the
// same shape back the other way.
self.onmessage = (e) => {
    const d = e.data;
    d.a[0] = 42;                       // a write through one view ...
    const report = {
        same: d.a.buffer === d.c.buffer,
        seen: d.c[0],                  // ... is seen through the other
        rawSame: d.raw ? d.raw === d.a.buffer : null,
        dvSame: d.dv ? d.dv.buffer === d.a.buffer : null,
        offsets: [d.a.byteOffset, d.c.byteOffset],
        other: d.other ? d.other.buffer !== d.a.buffer : null,
    };
    const back = new ArrayBuffer(8);
    report.x = new Uint8Array(back, 0, 4);
    report.y = new Uint8Array(back, 4, 4);
    self.postMessage(report);
};
