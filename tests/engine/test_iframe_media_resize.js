// Regression: an <iframe> sub-document's media viewport was fixed at creation.
// The element's box was refreshed every frame for LAYOUT, but nothing ever
// pushed the new size into the sub-document's media context — so resizing the
// iframe element re-laid-out the sub-document at the new size while its CSS
// @media rules and its realm's matchMedia lists stayed pinned to whatever the
// element measured when the sub-doc was built.
//
// The child paints two halves: the top from a CSS @media rule, the bottom from
// a matchMedia('change') listener. Both must react to the element resizing.

const CHILD = 'mql_resize_child';

function pixelAt(shot, fx, fy) {
  const x = Math.floor(shot.width * fx);
  const y = Math.floor(shot.height * fy);
  const i = ((y * shot.width) + x) * 4;
  return { r: shot.data[i], g: shot.data[i + 1], b: shot.data[i + 2] };
}
const show = (p) => `rgb(${p.r},${p.g},${p.b})`;

// --- narrow: below the 300px breakpoint -------------------------------------

const el = document.createElement('iframe');
el.setAttribute('src', CHILD);
el.style.width = '200px';
el.style.height = '120px';
document.body.appendChild(el);
flush();

let shot = el.capture();
assert(shot, 'sub-document rendered at the narrow size');

let css = pixelAt(shot, 0.5, 0.25);
let js = pixelAt(shot, 0.5, 0.75);
assert(css.r > 150 && css.b < 60,
    `narrow: CSS @media not matched, top half red (got ${show(css)})`);
assert(js.r > 30 && js.r < 100 && js.g > 30 && js.g < 100,
    `narrow: no matchMedia change yet, bottom half grey (got ${show(js)})`);

// --- widen past the breakpoint ----------------------------------------------

el.style.width = '400px';
flush();

shot = el.capture();
assert(shot, 'sub-document rendered at the wide size');

css = pixelAt(shot, 0.5, 0.25);
js = pixelAt(shot, 0.5, 0.75);
assert(css.b > 150 && css.r < 60,
    `wide: CSS @media re-evaluated, top half blue (got ${show(css)})`);
assert(js.g > 150 && js.r < 60 && js.b < 60,
    `wide: matchMedia fired change{matches:true}, bottom half green (got ${show(js)})`);

// --- an idle flush must not re-fire ------------------------------------------
// The per-realm generation gate means no size change => no event. A yellow
// bottom half is the child's "I got a redundant second change" marker.

flush();
flush();
shot = el.capture();
js = pixelAt(shot, 0.5, 0.75);
assert(js.g > 150 && js.r < 60,
    `idle flushes fire no redundant change (got ${show(js)})`);

// --- narrow again: the flip back is delivered too -----------------------------

el.style.width = '200px';
flush();

shot = el.capture();
css = pixelAt(shot, 0.5, 0.25);
js = pixelAt(shot, 0.5, 0.75);
assert(css.r > 150 && css.b < 60,
    `narrowed: CSS @media un-matched again, top half red (got ${show(css)})`);
// Second change (matches=false) => the child's redundant-change marker, yellow.
assert(js.r > 150 && js.g > 150 && js.b < 60,
    `narrowed: matchMedia delivered the flip back (got ${show(js)})`);

// --- height too, and teardown stays clean ------------------------------------

el.style.width = '400px';
el.style.height = '300px';
flush();
shot = el.capture();
assert(shot, 'sub-document rendered after a height change');

document.body.removeChild(el);
flush();
advanceTime(1200);
flush();

console.log('PASS: resizing an <iframe> re-evaluates the sub-document media ' +
    'viewport (CSS @media + matchMedia change)');
