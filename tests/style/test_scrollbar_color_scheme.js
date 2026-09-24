// Element scrollbars are translucent overlays themed by the element's used
// colour scheme, and `scrollbar-color` overrides them. The palette used to be
// written as 0..255 bytes into 0..1 float colours, which clamped to an opaque
// white strip on every page.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="dark" style="color-scheme:dark;width:200px;height:120px;overflow-y:scroll;background:rgb(20,20,30)"><div style="height:800px"></div></div>' +
    '<div id="light" style="color-scheme:light;width:200px;height:120px;overflow-y:scroll;background:rgb(240,240,240)"><div style="height:800px"></div></div>' +
    '<div id="custom" style="scrollbar-color: rgb(200, 0, 0) rgb(0, 0, 200);width:200px;height:120px;overflow-y:scroll;background:#000"><div style="height:800px"></div></div>';
flush();

// Sample the scrollbar column (5px wide, 1px in from the right edge), at a
// point on the track (below the thumb) and one on the thumb (at the top).
function bar(id, onThumb) {
    const r = document.getElementById(id).getBoundingClientRect();
    return getPixel(Math.floor(r.right - 4), Math.floor(onThumb ? r.top + 5 : r.bottom - 5));
}
const s = (p) => `rgb(${p.r},${p.g},${p.b})`;

let t = bar('dark', false), th = bar('dark', true);
assert(t.r < 90, 'dark track is a faint tint over the dark background, got ' + s(t));
assert(th.r > t.r + 20 && th.r < 230, 'dark thumb is a translucent light bar, got ' + s(th));

t = bar('light', false); th = bar('light', true);
assert(t.r > 180, 'light track keeps the light background, got ' + s(t));
assert(th.r < t.r - 20 && th.r > 40, 'light thumb is a translucent dark bar, got ' + s(th));

t = bar('custom', false); th = bar('custom', true);
assert(th.r > 150 && th.b < 60, 'scrollbar-color paints the thumb colour, got ' + s(th));
assert(t.b > 150 && t.r < 60, 'scrollbar-color paints the track colour, got ' + s(t));
