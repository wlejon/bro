// CSS Color 4 system colours (Canvas, CanvasText, ...) take the element's used
// colour scheme, as light-dark() does: its color-scheme weighed against the
// appearance.colorScheme preference. They resolve in the restyle pass to the
// scheme's rgb(), so getComputedStyle, paint and inheritance see the colour.

const style = document.createElement('style');
style.textContent = `
  .sys { width: 40px; height: 20px; background-color: Canvas; color: CanvasText; }
  #dark { color-scheme: dark; }
  #both { color-scheme: light dark; }
  #mix { width: 40px; height: 20px; background-color: color-mix(in srgb, Canvas 100%, red); }
  #alias { background-color: Window; }
  #border { border: 2px solid ButtonBorder; }
  #anim { animation-name: highlight; }
`;
document.head.appendChild(style);
document.body.innerHTML =
    '<div class="sys" id="plain"></div>' +
    '<div class="sys" id="dark"><span id="kid">x</span></div>' +
    '<div class="sys" id="both"></div>' +
    '<div id="mix"></div><div id="alias"></div><div id="border"></div><div id="anim"></div>';

const cs = (id) => getComputedStyle(document.getElementById(id));
function pixelAt(id) {
    const r = document.getElementById(id).getBoundingClientRect();
    return getPixel(Math.floor(r.left + r.width / 2), Math.floor(r.top + r.height / 2));
}
const WHITE = 'rgb(255, 255, 255)';
const DARK_CANVAS = 'rgb(18, 18, 18)';

// --- light preference -------------------------------------------------------
bro.settings.set('appearance.colorScheme', 'light');
flush();
assert(cs('plain').backgroundColor === WHITE, 'Canvas is white in the light scheme, got ' + cs('plain').backgroundColor);
assert(cs('plain').color === 'rgb(0, 0, 0)', 'CanvasText is black in the light scheme, got ' + cs('plain').color);
assert(cs('dark').backgroundColor === DARK_CANVAS,
    'color-scheme: dark picks the dark Canvas under a light preference, got ' + cs('dark').backgroundColor);
assert(cs('dark').color === WHITE, 'and the dark CanvasText, got ' + cs('dark').color);
assert(cs('kid').color === WHITE, 'a child inherits the resolved CanvasText, got ' + cs('kid').color);
assert(cs('both').backgroundColor === WHITE, '"light dark" follows the light preference, got ' + cs('both').backgroundColor);
assert(cs('mix').backgroundColor === WHITE, 'Canvas inside color-mix() resolves, got ' + cs('mix').backgroundColor);
assert(cs('alias').backgroundColor === WHITE, 'the deprecated Window aliases Canvas, got ' + cs('alias').backgroundColor);
assert(cs('border').borderTopColor === 'rgb(118, 118, 118)',
    'ButtonBorder in a border shorthand resolves, got ' + cs('border').borderTopColor);
assert(cs('anim').animationName === 'highlight',
    'a system colour name outside a colour property is left alone, got ' + cs('anim').animationName);
let px = pixelAt('plain');
assert(px.r > 240 && px.g > 240 && px.b > 240, `light Canvas paints white, got rgb(${px.r},${px.g},${px.b})`);
px = pixelAt('dark');
assert(px.r < 40 && px.g < 40 && px.b < 40, `dark Canvas paints near-black, got rgb(${px.r},${px.g},${px.b})`);

// --- dark preference: a restyle without a reload -------------------------------
bro.settings.set('appearance.colorScheme', 'dark');
flush();
assert(cs('plain').backgroundColor === WHITE,
    'color-scheme normal stays light under a dark preference, got ' + cs('plain').backgroundColor);
assert(cs('both').backgroundColor === DARK_CANVAS,
    '"light dark" follows the dark preference, got ' + cs('both').backgroundColor);
px = pixelAt('both');
assert(px.r < 40 && px.g < 40 && px.b < 40, `the flip repaints, got rgb(${px.r},${px.g},${px.b})`);
bro.settings.reset('appearance');

console.log('test_system_colors_scheme: OK');
