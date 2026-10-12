// What is under a pointer that has not moved changes when the page does, and
// the hover follows it as in a browser (Chrome's fake mouse move): :hover and
// the boundary events (pointerover/out, enter/leave, and the mouse ones),
// with no pointermove or mousemove, once after a layout, a DOM change or a
// scroll. The engine updated hover only on a move: an element replaced under
// a still pointer stayed un-hovered, so a hover effect or a tooltip on
// pointerover went away until the pointer moved ("hover drops if I hover too
// long" in an app that re-renders). Engine::recheckHover; the windowed loop's
// is tests/windowed/hover_recheck.

const step = (n = 3) => { for (let i = 0; i < n; ++i) advanceTime(16); };
document.body.style.margin = '0';
document.body.innerHTML = `<style>
  .b { position: absolute; left: 50px; top: 50px; width: 80px; height: 40px; }
  .b:hover { background: #f00; }
  #list { position: absolute; left: 300px; top: 50px; width: 100px; height: 60px; overflow: auto; }
  #list div { height: 30px; }
  #list div:hover { background: #0f0; }
</style><div id="host"><button class="b" id="b1">one</button></div>`;
step(5);

const events = [];
for (const t of ['pointerover', 'pointerout', 'pointerenter', 'pointerleave', 'mouseover', 'mouseout',
                 'mouseenter', 'mouseleave', 'pointermove', 'mousemove'])
    document.addEventListener(t, (e) => events.push(t + ' ' + (e.target.id || e.target.nodeName)), true);
const take = () => events.splice(0).join(', ');

mouseMove(90, 70);
step();
assert(document.getElementById('b1').matches(':hover'), 'the pointer hovers b1');
take();

// Replaced under the still pointer: the new element is :hover, its over and
// enter fire, and nothing moved.
document.getElementById('host').innerHTML = '<button class="b" id="b2">two</button>';
step();
const b2 = document.getElementById('b2');
assert(b2.matches(':hover'), 'the element put in under the pointer is :hover');
let got = take();
assert(got.includes('pointerover b2') && got.includes('mouseover b2') && got.includes('pointerenter b2'),
       `pointerover, mouseover and pointerenter fired on it (${got})`);
assert(!got.includes('move'), `no pointermove or mousemove: the pointer did not move (${got})`);
assert(getComputedStyle(b2).backgroundColor === 'rgb(255, 0, 0)', 'and it draws its :hover style');

// Moved away from under the pointer: out on it, over on what is under it now.
b2.style.left = '400px';
step();
got = take();
assert(!b2.matches(':hover'), 'an element moved away from the pointer is not :hover');
assert(got.includes('pointerout b2') && got.includes('pointerleave b2'), `it got out and leave (${got})`);

// Appearing under a still pointer.
const b3 = document.createElement('button');
b3.className = 'b';
b3.id = 'b3';
document.body.appendChild(b3);
step();
assert(b3.matches(':hover'), 'an element appearing under the pointer is :hover');
assert(take().includes('pointerover b3'), 'with a pointerover');

// A scroll under a still pointer: the row under it now is the one hovered.
b3.remove();
const list = document.createElement('div');
list.id = 'list';
for (let i = 0; i < 10; ++i) {
    const r = document.createElement('div');
    r.id = 'r' + i;
    r.textContent = 'row ' + i;
    list.appendChild(r);
}
document.body.appendChild(list);
step();
mouseMove(320, 60);
step();
assert(document.getElementById('r0').matches(':hover'), 'the pointer hovers the first row');
take();
list.scrollTop = 60;
step();
assert(document.getElementById('r2').matches(':hover'), 'scrolled two rows, the row now under it is :hover');
assert(!document.getElementById('r0').matches(':hover'), 'and the first is not');
got = take();
assert(got.includes('pointerover r2') && !got.includes('move'), `with its over and no move (${got})`);

// With a button down, the hover holds (as a drag would have it).
mouseDown(320, 60);
list.scrollTop = 0;
step();
assert(document.getElementById('r2').matches(':hover'), 'a button held: the hover stays until it is up');
mouseUp(320, 60);
list.remove();
step();

// An <iframe> under the pointer: an element replaced in it under a still
// pointer is :hover there.
const frame = document.createElement('iframe');
frame.setAttribute('src', 'hover_child');
frame.style.cssText = 'position: absolute; left: 0; top: 200px; width: 200px; height: 100px; border: 0';
document.body.appendChild(frame);
flush();
step();
const cw = frame.contentWindow;
mouseMove(60, 240);
step();
assert(cw.color() === 'rgb(255, 0, 0)', `the iframe's box under the pointer is :hover (${cw.color()})`);
cw.replace();
step();
assert(cw.color() === 'rgb(255, 0, 0)', `replaced under the still pointer, the iframe's new box is :hover (${cw.color()})`);

// A bro.window.open() window, by its own pointer.
const win = bro.window.open('hover_child', { width: 200, height: 100, title: 'hover' });
flush();
step();
mouseMove(60, 40, win.id);
step();
const red = (p) => p[0] > 200 && p[1] < 60 && p[2] < 60;
const at = (shot, x, y) => { const i = (y * shot.width + x) * 4; return [shot.data[i], shot.data[i + 1], shot.data[i + 2]]; };
let shot = win.capture();
assert(red(at(shot, 60, 40)), `the window's box under its pointer is :hover (${at(shot, 60, 40)})`);
win.postMessage('replace');
step(4);
shot = win.capture();
assert(red(at(shot, 60, 40)), `replaced under its still pointer, the window's new box is :hover (${at(shot, 60, 40)})`);
win.close();
console.log('hover recheck: replaced, appearing, moved away, scrolled, iframe and window');
