// A tab in preserved white space advances to the next tab stop (tab-size 8
// space advances) and paints nothing. It used to shape as the font's .notdef
// glyph: a missing-glyph box one glyph wide.

const root = document.getElementById('root');
root.innerHTML =
    '<pre id="tab" style="font:20px monospace;margin:0;display:inline-block;background:#fff;color:#000">a\tb</pre><br>' +
    '<pre id="sp" style="font:20px monospace;margin:0;display:inline-block">a       b</pre><br>' +
    '<pre id="two" style="font:20px monospace;margin:0;display:inline-block">ab\t\tc</pre><br>' +
    '<pre id="sp2" style="font:20px monospace;margin:0;display:inline-block">ab              c</pre>';
flush();

const w = (id) => document.getElementById(id).getBoundingClientRect().width;
assert(Math.abs(w('tab') - w('sp')) < 1,
    `"a<tab>b" is as wide as "a" + 7 spaces + "b": ${w('tab')} vs ${w('sp')}`);
assert(Math.abs(w('two') - w('sp2')) < 1,
    `two tabs after "ab" reach the second stop: ${w('two')} vs ${w('sp2')}`);

// Nothing is drawn in the tab's gap: every pixel between the end of "a" and
// the start of "b" is the white background.
const r = document.getElementById('tab').getBoundingClientRect();
const ch = r.width / 9;
let inked = 0;
// One getPixels() read of the element rather than a composite per sample.
const gapX = Math.floor(r.left), gapY = Math.floor(r.top);
const gap = getPixels(gapX, gapY, Math.ceil(r.right) - gapX, Math.ceil(r.bottom) - gapY);
const at = (x, y) => {
    const i = ((y - gapY) * gap.width + (x - gapX)) * 4;
    return { r: gap.data[i], g: gap.data[i + 1], b: gap.data[i + 2] };
};
for (let x = Math.ceil(r.left + ch * 1.3); x < r.left + ch * 7.7; x += 1) {
    for (let y = Math.ceil(r.top + 2); y < r.bottom - 2; y += 2) {
        const p = at(x, y);
        if (p.r < 200 || p.g < 200 || p.b < 200) inked++;
    }
}
assert(inked === 0, `the tab's gap paints nothing, found ${inked} inked pixels`);
