// SVG paints through one renderer: the native traversal (layout/svg_paint),
// inline and as an image. Each fixture in tests/layout/svg/ renders inline
// and through <img>, and is compared with reference renders made by Skia's
// SVG module (tests/layout/svg/ref/<name>.{inline,img}.png), except
// text_paint's, which is the traversal's own render checked against Chromium.
// The bar is visible equivalence, not identical pixels: a small mean
// difference and few pixels far off. Where the reference is wrong, the
// region is left out of the comparison and probed for the right answer
// instead. The inline and <img> renders of one fixture must agree with each
// other closely, as one traversal paints both. Text, textPath runs and every
// other child report real geometry.

const fs = require('fs');
const path = require('path');
const DIR = 'tests/layout/svg/';
const W = 160, H = 120;

document.body.style.cssText = 'margin:0;background:#ffffff';

function decodePng(file) {
    const img = bro.image.decodeOriented(new Uint8Array(fs.readFileSync(path.resolve(file))));
    const out = new Uint8Array(img.width * img.height * 4);
    for (let i = 0, n = img.width * img.height; i < n; i++)
        for (let c = 0; c < 4; c++) out[i * 4 + c] = c < img.channels ? img.pixels[i * img.channels + c] : 255;
    return { width: img.width, height: img.height, data: out };
}

function render(name) {
    const p = path.resolve(DIR + name + '.svg');
    document.body.innerHTML =
        `<div id=inl style="position:absolute;left:0;top:0;width:${W}px;height:${H}px">` +
        fs.readFileSync(p, 'utf8') + '</div>' +
        `<img id=im style="position:absolute;left:200px;top:0;width:${W}px;height:${H}px" src="` +
        p.replace(/\\/g, '/') + '">';
    // The <img> decodes off the page thread.
    for (let i = 0; i < 40; i++) {
        flush();
        if (document.getElementById('im').complete) break;
        advanceTime(16);
    }
    flush();
    return { inline: getPixels(0, 0, W, H), img: getPixels(200, 0, W, H) };
}

// Mean absolute RGB difference and the share of pixels off by more than 64 in
// any channel, outside the excluded rects.
function compare(a, b, exclude) {
    let sum = 0, far = 0, n = 0;
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            if (exclude.some((r) => x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3])) continue;
            const i = (y * W + x) * 4;
            let m = 0;
            for (let c = 0; c < 3; c++) {
                const d = Math.abs(a.data[i + c] - b.data[i + c]);
                sum += d;
                if (d > m) m = d;
            }
            if (m > 64) far++;
            n++;
        }
    }
    return { mean: sum / (n * 3), far: far / n };
}

const px = (shot, x, y) => {
    const i = (y * W + x) * 4;
    return [shot.data[i], shot.data[i + 1], shot.data[i + 2]];
};
const near = (c, want, tol = 40) => c.every((v, k) => Math.abs(v - want[k]) <= tol);

// Per fixture: rects [x, y, w, h] where the reference is wrong, for the
// inline and the <img> comparisons, and probes of what is right there.
const cases = {
    // The reference is the traversal's render, unhinted SVG text: against
    // Chromium's (Linux, Liberation Sans) it is mean 2.6, 1.3% far, where
    // Skia's SVG module's render was 4.6 and 3.2% from Chromium, outside
    // this test's bar.
    text_paint: {},
    text_path: { img: [[0, 30, W, 60]] },               // reference <img> lacks the <use href> guide
    mask: {},
    markers: { img: [[0, 0, W, H]] },                   // reference <img> lacks every marker
    filter_blur: {},
    // The reference renderer took flood-color as already linear in a linearRGB
    // filter, so its floods came out too light.
    filter_color: {
        both: [[100, 40, 40, 40], [0, 80, 80, 40]],
        probe(s) {
            // #ffd000 at 0.8 over white.
            assert(near(px(s, 120, 59), [255, 217, 51], 12), 'flood: ' + px(s, 120, 59));
        },
    },
    // The reference renderer has no feDropShadow.
    filter_shadow: {
        both: [[0, 0, 80, 60]],
        probe(s) {
            assert(px(s, 46, 46)[2] < 230 && px(s, 46, 46)[0] < 230, 'drop shadow under the box: ' + px(s, 46, 46));
            assert(near(px(s, 30, 20), [65, 105, 225], 30), 'box over its shadow: ' + px(s, 30, 20));
        },
    },
    // Old: no objectBoundingBox-unit pattern tiles, and a pattern stroke that
    // ignored the pattern's viewBox.
    pattern: {
        both: [[80, 0, 80, 120]],
        probe(s) {
            // dots: tiles of 18 x 13 user units from (84, 4), a dot at (6, 6) in each.
            assert(near(px(s, 90, 10), [224, 64, 64]), 'dot in the first tile: ' + px(s, 90, 10));
            assert(near(px(s, 108, 23), [224, 64, 64]), 'dot in the next tile: ' + px(s, 108, 23));
            assert(near(px(s, 99, 16), [255, 255, 255]), 'between dots: ' + px(s, 99, 16));
            // The pattern stroke paints green somewhere along its band.
            let green = 0;
            for (let x = 80; x < 160; x++) if (near(px(s, x, 64), [64, 160, 64])) green++;
            assert(green > 10, 'pattern stroke along the top edge: ' + green);
        },
    },
    // Old: <switch> unrendered inline; plain-href uses and switch in <img>.
    symbol_image: {
        inline: [[86, 56, 74, 64]],
        img: [[0, 0, W, H]],
        probe(s) {
            assert(near(px(s, 120, 72), [96, 176, 96]), 'switch renders its first child: ' + px(s, 120, 72));
            assert(near(px(s, 120, 100), [255, 255, 255]), 'and only that one: ' + px(s, 120, 100));
            assert(near(px(s, 24, 24), [224, 160, 0]), 'symbol through plain href: ' + px(s, 24, 24));
            assert(near(px(s, 134, 26), [224, 160, 0]), 'symbol keeps its own fill: ' + px(s, 134, 26));
        },
    },
    // Old: <image> drew nothing (no resource provider).
    image: {
        both: [[0, 0, W, H]],
        probe(s) {
            let nonWhite = 0;
            for (let y = 4; y < 60; y++) for (let x = 4; x < 60; x++) if (!near(px(s, x, y), [255, 255, 255], 8)) nonWhite++;
            assert(nonWhite > 500, 'the data: <image> paints: ' + nonWhite);
        },
    },
};

for (const name of Object.keys(cases)) {
    const c = cases[name];
    const shot = render(name);
    const refInline = decodePng(DIR + 'ref/' + name + '.inline.png');
    const refImg = decodePng(DIR + 'ref/' + name + '.img.png');
    const both = c.both || [];
    const ri = compare(shot.inline, refInline, both.concat(c.inline || []));
    const rm = compare(shot.img, refImg, both.concat(c.img || []));
    const self = compare(shot.inline, shot.img, []);
    console.log(`${name}: inline vs ref mean ${ri.mean.toFixed(2)} far ${(ri.far * 100).toFixed(2)}%, ` +
                `img vs ref mean ${rm.mean.toFixed(2)} far ${(rm.far * 100).toFixed(2)}%, ` +
                `inline vs img mean ${self.mean.toFixed(2)} far ${(self.far * 100).toFixed(2)}%`);
    if (!Number.isNaN(ri.mean)) assert(ri.mean < 4 && ri.far < 0.02, `${name} inline matches the reference`);
    if (!Number.isNaN(rm.mean)) assert(rm.mean < 4 && rm.far < 0.02, `${name} <img> matches the reference`);
    assert(self.mean < 1.5 && self.far < 0.005, `${name}: inline and <img> agree`);
    if (c.probe) { c.probe(shot.inline); c.probe(shot.img); }
}

// Geometry: text, tspans, textPath and the formerly fallback-only elements.
document.body.innerHTML =
    '<svg id=s width=200 height=200 style="position:absolute;left:10px;top:20px">' +
    '<defs><path id=cv d="M 10 150 Q 100 20 190 150"/><symbol id=sy viewBox="0 0 10 10"><rect width=10 height=10/></symbol></defs>' +
    '<text id=t x=20 y=40 font-family=Arial font-size=20>Hello <tspan id=ts fill=red>world</tspan></text>' +
    '<text id=tp font-family=Arial font-size=16><textPath id=tpp href="#cv">on a path</textPath></text>' +
    '<image id=i x=30 y=160 width=40 height=30 href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg=="/>' +
    '<use id=u href="#sy" x=100 y=160 width=20 height=20/>' +
    '</svg>';
flush();
const rect = (id) => document.getElementById(id).getBoundingClientRect();
const t = rect('t'), ts = rect('ts');
assert(t.width > 80 && t.width < 160 && t.height > 15 && t.height < 30, `text box ${t.width}x${t.height}`);
assert(Math.abs(t.left - 30) < 2, `text starts at x=20 in the svg: ${t.left}`);
assert(ts.left > t.left + 30 && ts.right <= t.right + 1 && ts.width > 30, `tspan inside its text: ${ts.left}..${ts.right}`);
const tp = rect('tpp');
assert(tp.width > 50 && tp.height > 15, `textPath has a box: ${tp.width}x${tp.height}`);
assert(tp.top > 20 + 20 && tp.bottom < 20 + 160, `textPath box follows the curve: ${tp.top}..${tp.bottom}`);
const im = rect('i');
assert(Math.abs(im.left - 40) < 1 && Math.abs(im.top - 180) < 1 && Math.abs(im.width - 40) < 1 && Math.abs(im.height - 30) < 1,
       `image box ${im.left},${im.top} ${im.width}x${im.height}`);
const u = rect('u');
assert(Math.abs(u.left - 110) < 1 && Math.abs(u.top - 180) < 1 && Math.abs(u.width - 20) < 1,
       `use of a symbol ${u.left},${u.top} ${u.width}x${u.height}`);

// <feImage> (#id element reference and data URI image) and <foreignObject> painting.
document.body.innerHTML =
    '<svg width=160 height=120 style="position:absolute;left:0;top:0">' +
    '<defs>' +
    '  <rect id=stamp x=0 y=0 width=40 height=40 fill="#00c040"/>' +
    '  <filter id=fi_ref x="0%" y="0%" width="100%" height="100%">' +
    '    <feImage href="#stamp" x=10 y=10 width=40 height=40/>' +
    '  </filter>' +
    '  <filter id=fi_uri x="0%" y="0%" width="100%" height="100%">' +
    '    <feImage href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==" x=60 y=10 width=40 height=40 preserveAspectRatio="none"/>' +
    '  </filter>' +
    '</defs>' +
    '<rect x=10 y=10 width=40 height=40 filter="url(#fi_ref)"/>' +
    '<rect x=60 y=10 width=40 height=40 filter="url(#fi_uri)"/>' +
    '<foreignObject x=10 y=60 width=100 height=40>' +
    '  <div style="width:80px;height:30px;background:#2060e0"></div>' +
    '</foreignObject>' +
    '</svg>';
for (let i = 0; i < 20; i++) { flush(); advanceTime(16); }
flush();
const shot2 = getPixels(0, 0, W, H);
assert(near(px(shot2, 25, 25), [0, 192, 64], 30), 'feImage #id reference paints green: ' + px(shot2, 25, 25));
assert(near(px(shot2, 75, 25), [255, 0, 0], 30), 'feImage data: URI paints red: ' + px(shot2, 75, 25));
assert(near(px(shot2, 30, 75), [32, 96, 224], 30), 'foreignObject HTML child paints blue: ' + px(shot2, 30, 75));

console.log('svg native: done');
