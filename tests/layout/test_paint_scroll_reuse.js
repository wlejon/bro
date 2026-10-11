// A document scroll moves every box by the same amount, so the paint cache
// replays the spans recorded before it under a translate rather than
// recording the page again (layout/paint_cache.h: SpanKey). What paints at a
// place of the viewport rather than of the document, the html/body background,
// is pinned: recorded again, never translated (a translated one would leave
// a strip of the viewport bare). Each scrolled frame is compared with the same
// frame recorded and rastered whole (setPaintCache(false)).

document.documentElement.style.background = '#203040';
document.body.style.margin = '0';
document.body.style.background = '#304050';
const colours = ['#c33', '#3a3', '#36c', '#cc3'];
for (let i = 0; i < 300; ++i) {
    const row = document.createElement('div');
    row.style.cssText = `height: 20px; margin: 2px 10px; background: ${colours[i % 4]}; border-radius: 4px`;
    const label = document.createElement('span');
    label.textContent = 'row ' + i;
    row.appendChild(label);
    document.body.appendChild(row);
}
flush();

function same(a, b) {
    if (!a || !b || a.width !== b.width || a.height !== b.height) return -1;
    const wa = new Uint32Array(a.data.buffer, a.data.byteOffset, a.data.length >> 2);
    const wb = new Uint32Array(b.data.buffer, b.data.byteOffset, b.data.length >> 2);
    let n = 0;
    for (let p = 0; p < wa.length; ++p) {
        if (wa[p] === wb[p]) continue;
        const i = p * 4;
        if (Math.abs(a.data[i] - b.data[i]) > 16 || Math.abs(a.data[i + 1] - b.data[i + 1]) > 16 ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > 16)
            ++n;
    }
    return n;
}

setPaintCache(true);
frameImage();
frameImage();
const before = perf.stats().paint;
for (const y of [37, 400, 1203, 900, 0, 2500, 2511]) {
    document.scrollingElement.scrollTop = y;
    flush();
    const cached = [frameImage(), frameImage()];
    setPaintCache(false);
    const full = frameImage();
    setPaintCache(true);
    for (let k = 0; k < 2; ++k) {
        const n = same(cached[k], full);
        assert(n === 0, `scrolled to ${y}, capture ${k + 1}: ${n} pixels differ from a full re-record`);
    }
}
const after = perf.stats().paint;
const scrolled = after.reusedScrolled - before.reusedScrolled;
const recorded = after.recorded - before.recorded;
assert(scrolled > 0, `spans were reused under a translate after a scroll (${scrolled})`);
assert(recorded < 50, `a scroll records almost nothing again: ${recorded} spans over 14 frames`);
console.log(`paint scroll reuse: ${scrolled} spans translated, ${recorded} recorded`);
