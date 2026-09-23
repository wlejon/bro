// Colour functions end to end: light-dark() picks its branch from the
// element's used colour scheme (color-scheme weighed against the
// appearance.colorScheme setting), and the CSS Color 4 functions and
// wide-gamut color() paint and report instead of falling back to black.
// Also: line-clamp's computed longhands, and :scope in querySelector,
// matches and closest.

const root = document.getElementById('root');

function pixelAt(el) {
    const r = el.getBoundingClientRect();
    return getPixel(Math.floor(r.left + r.width / 2), Math.floor(r.top + r.height / 2));
}

const style = document.createElement('style');
style.textContent = `
  .ld { width: 40px; height: 20px; background-color: light-dark(rgb(250, 0, 0), rgb(0, 0, 250)); }
  #only-dark { color-scheme: dark; }
  #both { color-scheme: light dark; }
  #nested { background-color: color-mix(in srgb, light-dark(rgb(255, 0, 0), rgb(0, 0, 255)) 100%, white); }
  #inherits { color-scheme: dark; color: light-dark(rgb(1, 2, 3), rgb(4, 5, 6)); }
  #oklch { width: 40px; height: 20px; background-color: oklch(62.8% 0.2577 29.23); }
  #p3 { width: 40px; height: 20px; background-color: color(display-p3 0 1 0); }
  #hsl-modern { width: 40px; height: 20px; background-color: hsl(240 100% 50% / 50%); }
`;
document.head.appendChild(style);
root.innerHTML =
    '<div class="ld" id="plain"></div>' +
    '<div class="ld" id="only-dark"></div>' +
    '<div class="ld" id="both"></div>' +
    '<div class="ld" id="nested"></div>' +
    '<div id="inherits"><span id="kid">x</span></div>' +
    '<div id="oklch"></div><div id="p3"></div><div id="hsl-modern"></div>';

const bg = (id) => getComputedStyle(document.getElementById(id)).backgroundColor;

// --- light preference -----------------------------------------------------
bro.settings.set('appearance.colorScheme', 'light');
flush();
assert(bg('plain') === 'rgb(250, 0, 0)', 'color-scheme normal is light, got ' + bg('plain'));
assert(bg('only-dark') === 'rgb(0, 0, 250)', 'color-scheme: dark is dark under a light preference, got ' + bg('only-dark'));
assert(bg('both') === 'rgb(250, 0, 0)', '"light dark" follows a light preference, got ' + bg('both'));
assert(bg('nested') === 'rgb(255, 0, 0)', 'light-dark() inside color-mix() resolves, got ' + bg('nested'));
let px = pixelAt(document.getElementById('plain'));
assert(px.r > 200 && px.b < 50, `light-dark() paints its light branch, got rgb(${px.r},${px.g},${px.b})`);
px = pixelAt(document.getElementById('only-dark'));
assert(px.b > 200 && px.r < 50, `color-scheme: dark paints the dark branch, got rgb(${px.r},${px.g},${px.b})`);
const kidColor = getComputedStyle(document.getElementById('kid')).color;
assert(kidColor === 'rgb(4, 5, 6)', 'a child inherits the resolved colour, got ' + kidColor);

// --- dark preference: restyles without a reload ------------------------------
bro.settings.set('appearance.colorScheme', 'dark');
flush();
assert(bg('plain') === 'rgb(250, 0, 0)', 'color-scheme normal stays light under a dark preference, got ' + bg('plain'));
assert(bg('both') === 'rgb(0, 0, 250)', '"light dark" follows a dark preference, got ' + bg('both'));
px = pixelAt(document.getElementById('both'));
assert(px.b > 200 && px.r < 50, `the flip repaints, got rgb(${px.r},${px.g},${px.b})`);
bro.settings.reset('appearance');

// --- wide-gamut and CSS Color 4 functions paint -----------------------------
px = pixelAt(document.getElementById('oklch'));
assert(px.r > 200 && px.g < 60 && px.b < 60, `oklch() red paints red, got rgb(${px.r},${px.g},${px.b})`);
assert(/^rgb\(2\d\d, \d{1,2}, \d{1,2}\)$/.test(bg('oklch')), 'oklch() reports as rgb(), got ' + bg('oklch'));
px = pixelAt(document.getElementById('p3'));
assert(px.g > 200 && px.r < 100 && px.b < 100, `display-p3 green gamut-maps to green, got rgb(${px.r},${px.g},${px.b})`);
assert(bg('hsl-modern') === 'rgba(0, 0, 255, 0.5)', 'modern hsl() with / alpha, got ' + bg('hsl-modern'));

// --- currentcolor in the other colour properties ------------------------------
root.insertAdjacentHTML('beforeend', '<div id="cc" style="color: rgb(9, 8, 7)">t</div>');
flush();
const cc = getComputedStyle(document.getElementById('cc'));
assert(cc.borderTopColor === 'rgb(9, 8, 7)', 'border-top-color currentcolor resolves, got ' + cc.borderTopColor);
assert(cc.textDecorationColor === 'rgb(9, 8, 7)', 'text-decoration-color currentcolor resolves, got ' + cc.textDecorationColor);

// --- line-clamp: shorthand over max-lines / block-ellipsis / continue ----------
root.insertAdjacentHTML('beforeend',
    '<div id="clamp" style="width:80px;font:16px/20px sans-serif;line-clamp:2">' +
    'one two three four five six seven eight nine ten eleven twelve</div>' +
    '<div id="noclamp">x</div>');
flush();
const clamp = document.getElementById('clamp');
let cs = getComputedStyle(clamp);
assert(cs.getPropertyValue('max-lines') === '2', 'max-lines longhand, got ' + cs.getPropertyValue('max-lines'));
assert(cs.getPropertyValue('continue') === 'collapse', 'continue longhand, got ' + cs.getPropertyValue('continue'));
assert(cs.getPropertyValue('block-ellipsis') === 'auto', 'block-ellipsis longhand, got ' + cs.getPropertyValue('block-ellipsis'));
assert(cs.getPropertyValue('line-clamp') === '2', 'line-clamp serializes from its longhands, got ' + cs.getPropertyValue('line-clamp'));
assert(getComputedStyle(document.getElementById('noclamp')).getPropertyValue('line-clamp') === 'none',
    'unclamped line-clamp is none');
const cr = clamp.getBoundingClientRect();
assert(Math.round(cr.height) === 40, 'two lines kept, got height ' + cr.height);
// The ellipsis run has no source text: hit-testing on it and selecting across
// it must stay inside the text node.
const hit = document.caretPositionFromPoint
    ? document.caretPositionFromPoint(cr.right - 4, cr.bottom - 10)
    : null;
if (hit) {
    assert(hit.offset >= 0 && hit.offset <= clamp.firstChild.data.length,
        'caret offset at the ellipsis stays inside the text, got ' + hit.offset);
}
const sel = window.getSelection();
sel.selectAllChildren(clamp);
const rects = sel.getRangeAt(0).getClientRects();
assert(rects.length > 0, 'selection over a clamped box has rects');
assert(sel.toString().indexOf('twelve') !== -1, 'selection text is the DOM text, clamped or not');
sel.removeAllRanges();

// --- :scope is the context element in the DOM query methods --------------------
root.insertAdjacentHTML('beforeend',
    '<section id="sc"><p class="a">1</p><div><p class="a">2</p></div></section>');
const sc = document.getElementById('sc');
assert(sc.querySelectorAll(':scope > p').length === 1, ':scope > p finds only the direct child');
assert(sc.querySelector(':scope > div > p').textContent === '2', ':scope > div > p');
assert(sc.matches(':scope'), 'el.matches(":scope") is true');
const firstP = sc.querySelector('p');
assert(!firstP.matches(':scope > p'), 'matches: :scope is the element itself, not its parent');
assert(firstP.closest(':scope') === firstP, 'closest(":scope") is the element itself');
assert(firstP.closest('section:has(> :scope)') === null || true, 'closest with :scope inside :has does not throw');
assert(firstP.closest(':scope section') === null, 'closest(":scope section"): no section under the element');
assert(document.querySelector(':root') === document.documentElement, ':root unchanged');

root.innerHTML = '';
style.remove();
console.log('test_color_functions: OK');
