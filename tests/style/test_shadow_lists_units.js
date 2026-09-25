// Every shadow of a text-shadow list paints, the first on top, and shadow
// lengths resolve their units: em against the element's font-size, rem
// against the root's, vw against the viewport, calc() through the length
// resolver.
//
// Before: text-shadow painted only its first shadow, and every shadow length
// was read as px whatever its unit (`2em` was 2px).

document.documentElement.style.fontSize = '20px';
document.body.style.cssText = 'margin:0;background:#fff;';
const box = (id, left, top, css) =>
    `<div id="${id}" style="position:absolute;left:${left}px;top:${top}px;` +
    `width:40px;height:40px;font-size:10px;color:rgb(255,0,0);${css}"></div>`;
document.body.innerHTML =
    // Each shadow is offset so that it covers [left+40, left+60) at least.
    box('em', 40, 40, 'box-shadow:2em 0;') +              // 20px
    box('rem', 140, 40, 'box-shadow:1rem 0;') +           // 20px (root 20px, not 10)
    box('vw', 240, 40, 'box-shadow:1.25vw 0;') +          // 24px of 1920
    box('calc', 340, 40, 'box-shadow:calc(1em + 10px) 0;') +
    box('ds', 440, 40, 'background:rgb(0,0,255);filter:drop-shadow(2em 0 0);') +
    // blur(1em) at font-size 10px is a 10px radius: it bleeds well past the
    // box edge, where blur(1px) would leave white.
    box('blur', 540, 40, 'background:rgb(0,0,255);filter:blur(1em);') +
    box('blurpx', 640, 40, 'background:rgb(0,0,255);filter:blur(1px);') +
    // Two text-shadows stacked 60px and 120px below the glyphs, and a pair on
    // the same spot where the first must be the one on top.
    `<div id="ts" style="position:absolute;left:40px;top:200px;font:bold 48px sans-serif;` +
    `line-height:60px;color:rgb(255,0,0);text-shadow:0 60px rgb(0,0,255), 0 120px;">MMMM</div>` +
    `<div id="top" style="position:absolute;left:400px;top:200px;font:bold 48px sans-serif;` +
    `line-height:60px;color:rgb(255,0,0);` +
    `text-shadow:0 60px rgb(0,0,255), 0 60px rgb(0,255,0);">MMMM</div>`;
flush();

function near(p, r, g, b, tol = 24) {
    return Math.abs(p.r - r) <= tol && Math.abs(p.g - g) <= tol && Math.abs(p.b - b) <= tol;
}
function rgb(p) { return 'rgb(' + p.r + ',' + p.g + ',' + p.b + ')'; }
function expectAt(x, y, want, msg) {
    const p = getPixel(x, y);
    assert(near(p, want[0], want[1], want[2]), msg + ': want rgb(' + want + '), got ' + rgb(p));
}

const RED = [255, 0, 0];
expectAt(90, 60, RED, 'box-shadow 2em at font-size 10px is 20px');
expectAt(190, 60, RED, 'box-shadow 1rem at a 20px root is 20px');
expectAt(290, 60, RED, 'box-shadow 1.25vw is 24px of a 1920px viewport');
expectAt(390, 60, RED, 'box-shadow calc(1em + 10px) is 20px');
expectAt(490, 60, RED, 'drop-shadow() 2em at font-size 10px is 20px');

// 5px outside the box's right edge, halfway down.
const bleed = getPixel(585, 60), bleedPx = getPixel(685, 60);
assert(bleed.r < 245 && bleed.b > bleed.r + 10,
       'blur(1em) bleeds 5px past the box, got ' + rgb(bleed));
assert(bleedPx.r > 250 && bleedPx.g > 250,
       'blur(1px) leaves 5px past the box white, got ' + rgb(bleedPx));

// Count samples of each colour in a band of rows under an element.
function band(id, from, to) {
    const r = document.getElementById(id).getBoundingClientRect();
    const counts = { red: 0, blue: 0, green: 0 };
    for (let y = Math.round(r.top) + from; y < Math.round(r.top) + to; y += 2) {
        for (let x = Math.round(r.left); x < Math.round(r.right); x += 2) {
            const p = getPixel(x, y);
            if (near(p, 255, 0, 0, 40)) counts.red++;
            else if (near(p, 0, 0, 255, 40)) counts.blue++;
            else if (near(p, 0, 255, 0, 40)) counts.green++;
        }
    }
    return counts;
}

const first = band('ts', 60, 120), second = band('ts', 120, 180);
assert(first.blue > 20 && first.red === 0,
       'the first text-shadow paints blue 60px down: ' + JSON.stringify(first));
assert(second.red > 20 && second.blue === 0,
       'the second text-shadow paints currentcolor 120px down: ' + JSON.stringify(second));

const stacked = band('top', 60, 120);
assert(stacked.blue > 20 && stacked.green === 0,
       'the first text-shadow paints on top of the second: ' + JSON.stringify(stacked));
