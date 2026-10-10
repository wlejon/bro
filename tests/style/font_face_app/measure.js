// Run by test_font_face_matching.js in a child bro-headless against this app.
// The app links ui/css/main.css, which @imports ui/css/tokens.css, which
// declares the TestInter family four times (Inter Regular/Medium/SemiBold/Bold
// at 400/500/600/700): two srcs relative to the sheet ("../fonts/..."), two
// through the /lib mount. Asserts that CSS font matching picks the face for
// each weight — layout measures the same face the painter draws — and that
// bold in a regular-only family is synthesized.

const root = document.getElementById('root');
const TEXT = 'Hamburgefonstiv 0123';

function span(family, weight, text) {
  const s = document.createElement('span');
  s.className = 'm';
  s.style.fontFamily = family;
  s.style.fontWeight = String(weight);
  s.textContent = text || TEXT;
  root.appendChild(s);
  root.appendChild(document.createElement('br'));
  return s;
}
const width = (el) => el.getBoundingClientRect().width;
const fam = '"TestInter", monospace';

// The family loaded at all: a monospace fallback gives "iiiiii" and "WWWWWW"
// the same width; Inter is proportional.
const narrow = width(span(fam, 400, 'iiiiii'));
const wide = width(span(fam, 400, 'WWWWWW'));
assert(wide > narrow * 2,
       'TestInter loaded from the sheet-relative src (iiiiii ' + narrow.toFixed(2) +
       ' vs WWWWWW ' + wide.toFixed(2) + '; equal means the monospace fallback)');

const w = {};
for (const weight of [100, 300, 400, 450, 500, 550, 600, 650, 700, 900]) {
  w[weight] = width(span(fam, weight));
}
console.log('MEASURE_JSON ' + JSON.stringify(w));
const show = (a, b) => a + '=' + w[a].toFixed(2) + ' ' + b + '=' + w[b].toFixed(2);
const same = (a, b) => Math.abs(w[a] - w[b]) < 0.01;

// One face per declared weight, each a different width: the bug drew the
// first-declared face (Regular) for every weight.
assert(w[400] < w[500], 'Medium is wider than Regular: ' + show(400, 500));
assert(w[500] < w[600], 'SemiBold is wider than Medium: ' + show(500, 600));
assert(w[600] < w[700], 'Bold is wider than SemiBold: ' + show(600, 700));

// CSS Fonts 4 §5.2 for weights between the declared ones.
assert(same(100, 400), 'below 400 with nothing lighter: the lightest face (400): ' + show(100, 400));
assert(same(300, 400), '300 -> 400: ' + show(300, 400));
assert(same(450, 500), '450 -> 500 (desired..500 ascending first): ' + show(450, 500));
assert(same(550, 600), '550 -> 600 (heavier first above 500): ' + show(550, 600));
assert(same(650, 700), '650 -> 700: ' + show(650, 700));
assert(same(900, 700), '900 -> 700 (nothing heavier, so lighter descending): ' + show(900, 700));

// Synthetic bold: a family with only a regular face draws weight 700 with
// the regular outlines emboldened — more ink than weight 400, not the same.
root.textContent = '';
const reg = span('"TestInterRegularOnly", monospace', 400, 'HHHH');
const syn = span('"TestInterRegularOnly", monospace', 700, 'HHHH');
flush();
function ink(el) {
  const r = el.getBoundingClientRect();
  const px = getPixels(Math.floor(r.x), Math.floor(r.y), Math.ceil(r.width), Math.ceil(r.height));
  let sum = 0;
  for (let i = 0; i < px.data.length; i += 4) sum += 255 - px.data[i];
  return sum;
}
const inkReg = ink(reg), inkSyn = ink(syn);
assert(inkSyn > inkReg * 1.15,
       'weight 700 in a regular-only family is synthesized bold: ink ' + inkSyn +
       ' vs regular ' + inkReg);
console.log('font-face matching: ok');
