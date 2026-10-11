// Element scrollbars are drawn from the document's scroll containers
// (Document::scrollContainers, kept as each element's style is set) rather
// than from a walk of the whole document. A bar shows exactly where a walk
// would have drawn one: a box that starts or stops scrolling gains or loses
// it, one under display:none has none until shown, one slotted into a shadow
// tree is placed through the slot's parent, a scrolled ancestor moves it, and
// a removed one is gone.

document.body.style.margin = '0';
document.body.style.background = '#000';

const BLUE = [0, 0, 255];
function isBlue(x, y) {
    const p = getPixel(x, y);
    return Math.abs(p.r - BLUE[0]) <= 2 && Math.abs(p.g - BLUE[1]) <= 2 && Math.abs(p.b - BLUE[2]) <= 2;
}
// A 100x80 box at (left, top) holding a 300px-tall blue block. Its right
// edge, a few px in, shows the block unless a vertical bar covers it.
function makeBox(left, top, overflow) {
    const box = document.createElement('div');
    box.style.cssText = `position:absolute; left:${left}px; top:${top}px; width:100px; height:80px; overflow:${overflow}`;
    const inner = document.createElement('div');
    inner.style.cssText = 'height:300px; background:#00f';
    box.appendChild(inner);
    return box;
}
// A bar: neither the blue block nor the black page shows there.
function barAt(left, top) {
    const p = getPixel(left + 96, top + 30);
    const black = p.r <= 2 && p.g <= 2 && p.b <= 2;
    return !black && !isBlue(left + 96, top + 30);
}

// A scroller from the start: a bar.
const a = makeBox(10, 10, 'auto');
document.body.appendChild(a);
flush();
assert(barAt(10, 10), 'overflow:auto with overflowing content shows a bar');

// One that starts clipping-free gains a bar when it starts scrolling, and
// loses it when it stops.
const b = makeBox(130, 10, 'hidden');
document.body.appendChild(b);
flush();
assert(!barAt(130, 10), 'overflow:hidden shows no bar');
b.style.overflow = 'scroll';
flush();
assert(barAt(130, 10), 'switched to overflow:scroll, it shows a bar');
b.style.overflow = 'hidden';
flush();
assert(!barAt(130, 10), 'switched back to hidden, the bar is gone');

// Under display:none: no bar until the ancestor shows.
const hider = document.createElement('div');
hider.style.display = 'none';
const c = makeBox(250, 10, 'auto');
hider.appendChild(c);
document.body.appendChild(hider);
flush();
assert(!barAt(250, 10), 'a scroller under display:none draws no bar');
hider.style.display = 'block';
flush();
assert(barAt(250, 10), 'shown, it draws its bar');

// Slotted into a shadow tree, placed through the slot's parent.
const host = document.createElement('div');
host.style.cssText = 'position:absolute; left:10px; top:120px';
const shadow = host.attachShadow({ mode: 'open' });
shadow.innerHTML = '<div style="padding-left:30px; padding-top:20px"><slot></slot></div>';
const d = makeBox(0, 0, 'auto');
d.style.position = 'static';
host.appendChild(d);
document.body.appendChild(host);
flush();
assert(barAt(10 + 30, 120 + 20), 'a slotted scroller draws its bar where the slot put it');

// A light child no slot takes is not rendered, and has no bar.
const host2 = document.createElement('div');
host2.style.cssText = 'position:absolute; left:250px; top:120px; width:100px; height:80px; background:#00f';
host2.attachShadow({ mode: 'open' }).innerHTML = '<div style="height:80px"></div>';
const e = makeBox(0, 0, 'auto');
e.style.position = 'static';
host2.appendChild(e);
document.body.appendChild(host2);
flush();
assert(!barAt(250, 120), 'an unslotted light child draws no bar');

// Inside a scrolled ancestor: the bar moves with its box.
const outer = document.createElement('div');
outer.style.cssText = 'position:absolute; left:130px; top:120px; width:110px; height:160px; overflow:hidden';
const spacer = document.createElement('div');
spacer.style.height = '50px';
outer.appendChild(spacer);
const f = makeBox(0, 0, 'auto');
f.style.position = 'static';
outer.appendChild(f);
const tail = document.createElement('div');
tail.style.height = '200px';
outer.appendChild(tail);
document.body.appendChild(outer);
flush();
assert(barAt(130, 120 + 50), 'a nested scroller draws its bar in place');
outer.scrollTop = 40;
flush();
assert(barAt(130, 120 + 10), 'scrolled by its ancestor, the bar moves with it');

// Removed: no bar, nothing stale.
a.remove();
flush();
assert(!barAt(10, 10), 'a removed scroller leaves no bar');
console.log('scrollbar registry: checked');
