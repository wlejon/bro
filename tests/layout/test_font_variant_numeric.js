// font-variant-numeric / font-feature-settings select OpenType features, and
// layout measures text with the same features paint shapes it with: a
// `tabular-nums` run of "111" is exactly as wide as one of "000" in a font
// whose default digits are proportional, so right-aligned counters line up
// without a monospace font.
//
// Needs an installed font with proportional default digits AND a `tnum`
// feature. The candidates below all carry `tnum`; one qualifies when its plain
// "111" and "000" measure differently, which does not depend on the feature
// under test, so a broken feature fails rather than skips.

const CANDIDATES = [
    'Bahnschrift', 'Segoe UI Variable Text', 'Candara', 'Constantia', 'Corbel',
    'Sitka Text',                                   // Windows
    'Inter', 'Cantarell', 'Source Sans 3', 'Source Sans Pro', 'Fira Sans',
    'Open Sans',                                    // Linux, if installed
    'Avenir Next', 'Optima',                        // macOS
];

const root = document.getElementById('root');
const width = (id) => document.getElementById(id).getBoundingClientRect().width;
const close = (a, b) => Math.abs(a - b) < 0.01;

// Two spans, "111" and "000", in `family` with `style` on each span (or on
// their parent when `onParent`).
function measure(family, style, onParent = false) {
    const own = onParent ? '' : style;
    root.innerHTML =
        `<div id="p" style="font-family:'${family}';font-size:40px;${onParent ? style : ''}">` +
        `<span id="ones" style="${own}">111</span><br>` +
        `<span id="zeros" style="${own}">000</span></div>`;
    flush();
    return { ones: width('ones'), zeros: width('zeros') };
}

let family = null;
for (const f of CANDIDATES) {
    const m = measure(f, '');
    if (!close(m.ones, m.zeros)) { family = f; break; }
}

if (!family) {
    skipTest('no installed font with proportional digits and tnum among: ' + CANDIDATES.join(', '));
} else {
    console.log('font with proportional digits + tnum: ' + family);

    const plain = measure(family, '');
    assert(!close(plain.ones, plain.zeros),
           `${family}: default digits are proportional (${plain.ones} vs ${plain.zeros})`);

    // font-variant-numeric: tabular-nums
    const tab = measure(family, 'font-variant-numeric:tabular-nums');
    assert(close(tab.ones, tab.zeros),
           `tabular-nums: "111" and "000" measure the same, got ${tab.ones} vs ${tab.zeros}`);

    // proportional-nums is the default in this font, so it measures as plain.
    const prop = measure(family, 'font-variant-numeric:proportional-nums');
    assert(close(prop.ones, plain.ones) && close(prop.zeros, plain.zeros),
           `proportional-nums measures as the default: ${prop.ones}/${prop.zeros} vs ${plain.ones}/${plain.zeros}`);

    // font-feature-settings: "tnum" (on by default), and explicitly off.
    // (Single-quoted CSS strings: the style goes into a double-quoted attribute.)
    const ffs = measure(family, "font-feature-settings:'tnum'");
    assert(close(ffs.ones, ffs.zeros),
           `font-feature-settings "tnum": same width, got ${ffs.ones} vs ${ffs.zeros}`);
    assert(close(ffs.ones, tab.ones), `"tnum" matches tabular-nums: ${ffs.ones} vs ${tab.ones}`);
    const off = measure(family, "font-variant-numeric:tabular-nums;font-feature-settings:'tnum' off");
    assert(close(off.ones, plain.ones),
           `font-feature-settings "tnum" off overrides tabular-nums: ${off.ones} vs ${plain.ones}`);

    // Inheritance: set on the parent, the spans' text is tabular.
    const inh = measure(family, 'font-variant-numeric:tabular-nums', true);
    assert(close(inh.ones, inh.zeros),
           `tabular-nums inherits: ${inh.ones} vs ${inh.zeros}`);
    const inhFfs = measure(family, "font-feature-settings:'tnum' on", true);
    assert(close(inhFfs.ones, inhFfs.zeros),
           `font-feature-settings inherits: ${inhFfs.ones} vs ${inhFfs.zeros}`);
    const childFfs = getComputedStyle(document.getElementById('ones'))
        .getPropertyValue('font-feature-settings');
    assert(childFfs.includes('tnum'),
           'the child computes the inherited font-feature-settings, got ' + childFfs);

    // A child can turn it back off inside a tabular parent.
    root.innerHTML =
        `<div style="font-family:'${family}';font-size:40px;font-variant-numeric:tabular-nums">` +
        `<span id="ones" style="font-variant-numeric:normal">111</span><br>` +
        `<span id="zeros">000</span></div>`;
    flush();
    assert(close(width('ones'), plain.ones),
           `a child's normal undoes the parent's tabular-nums: ${width('ones')} vs ${plain.ones}`);

    // Mixed in one line: a tabular counter inside proportional text measures
    // tabular (the inline context carries each span's own features), and the
    // Range rect over its text (caret geometry) agrees with the layout box.
    root.innerHTML =
        `<div style="font-family:'${family}';font-size:40px">Count ` +
        `<span id="ones" style="font-variant-numeric:tabular-nums">111</span> of ` +
        `<span id="zeros" style="font-variant-numeric:tabular-nums">000</span></div>`;
    flush();
    assert(close(width('ones'), width('zeros')),
           `inline tabular counters: ${width('ones')} vs ${width('zeros')}`);
    assert(close(width('ones'), tab.ones), `and as wide as alone: ${width('ones')} vs ${tab.ones}`);
    const range = document.createRange();
    range.selectNodeContents(document.getElementById('ones').firstChild);
    const rw = range.getBoundingClientRect().width;
    assert(Math.abs(rw - width('ones')) < 0.5,
           `Range over the tabular text matches its box: ${rw} vs ${width('ones')}`);

    // Right-aligned column: a shrink-wrapped tabular block is as wide for
    // every count, so the right edges line up and the left edges do too.
    root.innerHTML =
        `<div style="font-family:'${family}';font-size:40px;font-variant-numeric:tabular-nums;` +
        `text-align:right;width:300px">` +
        `<div><span id="ones">111</span></div><div><span id="zeros">000</span></div></div>`;
    flush();
    const r1 = document.getElementById('ones').getBoundingClientRect();
    const r0 = document.getElementById('zeros').getBoundingClientRect();
    assert(Math.abs(r1.left - r0.left) < 0.01 && Math.abs(r1.right - r0.right) < 0.01,
           `right-aligned tabular digits line up: [${r1.left}, ${r1.right}] vs [${r0.left}, ${r0.right}]`);
}

root.innerHTML = '';
