// A position:fixed box stays put while the viewport scrolls: it paints at
// the same place, clicks land on it there, and its client rect does not
// move. It used to scroll with the page in all three once element rects
// began honouring the viewport scroll. A fixed box inside a scrolled
// overflow container ignores that container's scroll as well, and a fixed
// box under a transformed ancestor (which takes the containing-block job
// from the viewport) still moves with the page.
//
// Nothing but <html> lies under the bar (the body is a narrow column off to
// the right), so elementFromPoint has only the fixed box to find there; it
// used to fall back to a tree-order search of its own that disagreed with
// the click hit test. A pointer-events:none fixed box is not a hit either.

const root = document.getElementById('root');
document.body.style.height = 'auto';
document.body.style.width = '40px';
document.body.style.marginLeft = '1200px';
root.innerHTML =
    '<div id="bar" style="position:fixed;top:10px;left:20px;width:200px;height:50px;background:rgb(255,0,0)"></div>' +
    '<div id="ghost" style="position:fixed;top:900px;left:20px;width:200px;height:50px;pointer-events:none"></div>' +
    '<div style="height:5000px"></div>' +
    '<div id="box" style="height:300px;overflow:auto">' +
    '  <div style="height:2000px">' +
    '    <div id="inner" style="position:fixed;top:100px;left:300px;width:80px;height:40px;background:rgb(0,0,255)"></div>' +
    '  </div>' +
    '</div>' +
    '<div style="transform:translateX(0px)">' +
    '  <div id="held" style="position:fixed;top:700px;left:20px;width:50px;height:20px;background:rgb(0,128,0)"></div>' +
    '</div>';
flush();

const bar = document.getElementById('bar');
const inner = document.getElementById('inner');
const held = document.getElementById('held');
let clicks = 0;
bar.addEventListener('click', () => ++clicks);

function near(a, b, what) {
    assert(Math.abs(a - b) < 1, what + ': expected ' + b + ', got ' + a);
}

near(bar.getBoundingClientRect().top, 10, 'unscrolled, the bar is at its top');
const heldTop0 = held.getBoundingClientRect().top;

window.scrollTo(0, 400);
document.getElementById('box').scrollTop = 500;
flush();
near(window.scrollY, 400, 'the viewport scrolled');

// Client rects.
const r = bar.getBoundingClientRect();
near(r.top, 10, 'scrolled, the bar keeps its client top');
near(r.left, 20, 'and its left');
near(inner.getBoundingClientRect().top, 100,
     'a fixed box in a scrolled container ignores both scrolls');
near(held.getBoundingClientRect().top, heldTop0 - 400,
     'a fixed box under a transform moves with the page');

// Hit testing.
assert(document.elementFromPoint(30, 20) === bar,
       'elementFromPoint finds the bar where it sits, got ' +
       (document.elementFromPoint(30, 20) || {}).id);
assert(document.elementFromPoint(310, 110) === inner, 'and the inner fixed box');
assert(document.elementFromPoint(30, 910) === document.documentElement,
       'a pointer-events:none fixed box is not hit, got ' +
       (document.elementFromPoint(30, 910) || {}).id);
assert(document.elementFromPoint(600, 600) === document.documentElement,
       'empty viewport answers <html>');
click(30, 20);
flush();
assert(clicks === 1, 'a click at the bar reaches it, clicks ' + clicks);

// Paint.
const p = getPixel(30, 20);
assert(p.r > 200 && p.g < 50 && p.b < 50, 'the bar paints where it sits: ' + JSON.stringify(p));
const q = getPixel(310, 110);
assert(q.b > 200 && q.r < 50, 'so does the inner fixed box: ' + JSON.stringify(q));

window.scrollTo(0, 0);
document.body.style.height = '';
document.body.style.width = '';
document.body.style.marginLeft = '';
root.innerHTML = '';
