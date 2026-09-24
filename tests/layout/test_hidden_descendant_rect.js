// An element inside a subtree that has just become display:none has no box:
// its getBoundingClientRect is empty, not the rect of its last visible layout.

const root = document.getElementById('root');
root.innerHTML =
    '<style>.folded .body{display:none}</style>' +
    '<div id="panel"><div class="body" id="body"><button id="btn">Click me</button>' +
    '<span id="txt">text</span></div></div>';
flush();

const btn = document.getElementById('btn');
const before = btn.getBoundingClientRect();
assert(before.width > 0 && before.height > 0, 'visible button has a box');

document.getElementById('panel').className = 'folded';
flush();
const body = document.getElementById('body').getBoundingClientRect();
assert(body.width === 0 && body.height === 0, 'the hidden body has no box');
const after = btn.getBoundingClientRect();
assert(after.width === 0 && after.height === 0 && after.x === 0 && after.y === 0,
       'the button inside it has no box either, got ' + after.width + 'x' + after.height);
assert(btn.offsetWidth === 0 && btn.offsetHeight === 0, 'offsetWidth/Height are 0');
const range = document.createRange();
range.selectNodeContents(document.getElementById('txt'));
const rects = Array.from(range.getClientRects()).filter(r => r.width > 0);
assert(rects.length === 0, 'hidden text has no range rects');

document.getElementById('panel').className = '';
flush();
const again = btn.getBoundingClientRect();
assert(Math.abs(again.width - before.width) < 0.5 && again.height > 0,
       'shown again, the button has its box back');

root.innerHTML = '';
