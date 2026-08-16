// `body { overflow: hidden }` must not stop clicks reaching the page.
//
// CSS 2.1 §11.1.1: the root element's overflow propagates to the viewport, and
// <body> donates in its place when the root is `visible`. The donor keeps
// behaving as `visible` itself — the viewport does the clipping.
//
// Hit testing used to take body's `hidden` literally. That is invisible on an
// ordinary page, where body's box covers its content anyway, and fatal on an
// app that positions everything out of flow: body's own box is then zero-height,
// so the first step of the hit test rejected the entire document. The stock
// three.js editor painted perfectly and ignored every click, menu, tab and
// checkbox in it.

const root = document.getElementById('root');

// The editor's shape: a body that clips, with nothing in flow to give it a
// box. (The test app's stylesheet sizes body explicitly, so height is reset
// here rather than left to the default `auto` — the point is the zero-height
// box, however it arises.)
const savedHeight = document.body.style.height;
document.body.style.margin = '0';
document.body.style.height = '0';
document.body.style.overflow = 'hidden';

// Out-of-flow, so it contributes nothing to body's height — and placed well
// below wherever body's own box ends.
const panel = document.createElement('div');
panel.id = 'oof-panel';
panel.style.cssText =
    'position:absolute;left:0;top:400px;width:300px;height:120px;background:#333;';
document.body.appendChild(panel);
flush();

const bodyRect = document.body.getBoundingClientRect();
const panelRect = panel.getBoundingClientRect();

// The premise of the test: the click lands outside body's own border box.
assert(bodyRect.top + bodyRect.height < panelRect.top,
       'the panel sits below body\'s own box (body h=' + bodyRect.height +
       ', panel top=' + panelRect.top + ')');
assert(getComputedStyle(document.body).overflow === 'hidden',
       'body still computes overflow:hidden');

let hits = 0;
let lastTarget = null;
panel.addEventListener('click', function (e) { hits++; lastTarget = e.target; });

const cx = (panelRect.left + panelRect.width / 2) | 0;
const cy = (panelRect.top + panelRect.height / 2) | 0;
click(cx, cy);

assert(hits === 1, 'the click reached the out-of-flow panel, got ' + hits + ' hits');
assert(lastTarget === panel, 'and targeted the panel itself');

// A descendant of the panel is reachable too — the whole subtree was being
// pruned, not just the top box.
const inner = document.createElement('button');
inner.textContent = 'go';
inner.style.cssText = 'position:absolute;left:20px;top:20px;width:80px;height:30px;';
panel.appendChild(inner);
flush();

let innerHits = 0;
inner.addEventListener('click', function () { innerHits++; });
const ir = inner.getBoundingClientRect();
click((ir.left + ir.width / 2) | 0, (ir.top + ir.height / 2) | 0);
assert(innerHits === 1, 'a descendant of the panel is hittable, got ' + innerHits);
assert(hits === 2, 'and the click still bubbles to the panel');

// An element that really is outside every box stays a miss — the fix must not
// turn body into a catch-all.
let strayHits = 0;
document.addEventListener('click', function (e) {
    if (e.target === panel || panel.contains(e.target)) return;
    strayHits++;
});
click((panelRect.left + panelRect.width + 200) | 0, (panelRect.top + 10) | 0);
assert(hits === 2, 'a click beside the panel does not reach it');

// A genuine clipper still clips: give the panel its own overflow and a child
// hanging outside it becomes unhittable, exactly as before.
panel.style.overflow = 'hidden';
const escapee = document.createElement('div');
escapee.style.cssText =
    'position:absolute;left:0;top:200px;width:100px;height:40px;background:#f00;';
panel.appendChild(escapee);
flush();

let escapeeHits = 0;
escapee.addEventListener('click', function () { escapeeHits++; });
const er = escapee.getBoundingClientRect();
click((er.left + er.width / 2) | 0, (er.top + er.height / 2) | 0);
assert(escapeeHits === 0,
       'overflow:hidden on an ordinary element still clips hit testing, got ' +
       escapeeHits);

document.body.removeChild(panel);
document.body.style.overflow = '';
document.body.style.height = savedHeight;
root.innerHTML = '';

console.log('PASS: body overflow propagates to the viewport for hit testing');
