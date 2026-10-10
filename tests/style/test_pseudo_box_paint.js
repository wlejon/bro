// ::before / ::after boxes paint like elements: the element painter paints
// them, so box-shadow (outset and inset), gradient backgrounds, rounded
// corners and non-uniform borders all show on a generated box.
//
// Before: drawPseudo painted only background-color and uniform solid borders,
// so every one of the probes below came back white (or a square corner).

document.head.insertAdjacentHTML('beforeend', `<style>
  .host { position:absolute; width:60px; height:60px; }
  .host::after { content:""; position:absolute; left:0; top:0; width:60px; height:60px; }
  #shadow { left:20px; top:20px; }
  #shadow::after { background:rgb(255,255,255); box-shadow:20px 0 0 0 rgb(255,0,0); }
  #inset { left:140px; top:20px; }
  #inset::after { background:rgb(255,255,255); box-shadow:inset 0 0 0 10px rgb(0,0,255); }
  #grad { left:240px; top:20px; }
  #grad::before { content:""; position:absolute; left:0; top:0; width:60px; height:60px;
                  background:linear-gradient(to right, rgb(255,0,0), rgb(0,0,255)); }
  #round { left:340px; top:20px; }
  #round::after { background:rgb(0,128,0); border-radius:30px; }
  #sides { left:440px; top:20px; }
  #sides::after { box-sizing:border-box; border-style:solid; border-color:rgb(255,0,0) rgb(0,0,255);
                  border-width:12px 4px; }
  #dashed { left:20px; top:120px; }
  #dashed::after { box-sizing:border-box; border:6px dashed rgb(0,0,0); }
</style>`);
document.body.style.cssText = 'margin:0;background:#fff;';
document.body.innerHTML = ['shadow', 'inset', 'grad', 'round', 'sides', 'dashed']
    .map((id) => `<div class="host" id="${id}"></div>`).join('');
flush();

function near(p, r, g, b, tol = 24) {
    return Math.abs(p.r - r) <= tol && Math.abs(p.g - g) <= tol && Math.abs(p.b - b) <= tol;
}
function rgb(p) { return 'rgb(' + p.r + ',' + p.g + ',' + p.b + ')'; }
function expectAt(x, y, want, msg) {
    const p = getPixel(x, y);
    assert(near(p, want[0], want[1], want[2]), msg + ': want rgb(' + want + '), got ' + rgb(p));
}

// Outset box-shadow: 20px to the right of the 60px box (x 80..100).
expectAt(90, 50, [255, 0, 0], '::after outset box-shadow');
expectAt(50, 50, [255, 255, 255], '::after background over its shadow');

// Inset box-shadow: a 10px blue band inside the white box.
expectAt(145, 50, [0, 0, 255], '::after inset box-shadow');
expectAt(170, 50, [255, 255, 255], '::after inset shadow leaves the middle');

// Gradient background: red at the left end, blue at the right.
{
    const l = getPixel(243, 50), r = getPixel(297, 50);
    assert(l.r > 200 && l.b < 60, '::before gradient starts red, got ' + rgb(l));
    assert(r.b > 200 && r.r < 60, '::before gradient ends blue, got ' + rgb(r));
}

// Rounded corners: the corner of a 30px radius is outside the circle.
expectAt(342, 22, [255, 255, 255], '::after border-radius cuts the corner');
expectAt(370, 50, [0, 128, 0], '::after rounded box fills its middle');

// Non-uniform borders: 12px red top/bottom, 4px blue left/right.
expectAt(470, 25, [255, 0, 0], '::after 12px top border');
expectAt(470, 30, [255, 0, 0], '::after top border is 12px wide (y 20..32)');
expectAt(442, 50, [0, 0, 255], '::after 4px left border');
expectAt(470, 50, [255, 255, 255], '::after content inside the borders');

// Dashed: the border is drawn dashed, so the top edge has gaps.
{
    const px = getPixels(20, 122, 60, 1);
    let dark = 0, light = 0;
    for (let i = 0; i < 60; i++) {
        const v = px.data[i * 4];
        if (v < 80) dark++; else if (v > 200) light++;
    }
    assert(dark > 10 && light > 6, '::after dashed border has dashes and gaps, dark=' + dark + ' light=' + light);
}

console.log('test_pseudo_box_paint.js PASSED');
