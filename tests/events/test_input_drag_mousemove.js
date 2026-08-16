// A drag that starts on a text input must still reach the page as mousemove.
//
// Pressing inside an <input> starts the control's own selection drag, and the
// engine used to consume every move from then on — the page saw mousedown, then
// mouseup, and nothing in between. That kills the drag-to-scrub number field:
// mousedown on the input, read document mousemove until mouseup, turn the
// horizontal delta into a value. It is how the three.js editor's position /
// rotation / scale fields work, and lil-gui's and dat.GUI's before them.
//
// Both halves have to hold: the page gets its moves, and the control still
// extends its own selection.

const root = document.getElementById('root');
root.innerHTML =
    '<input id="num" type="text" value="0.000" ' +
    'style="position:absolute;left:20px;top:40px;width:120px;height:24px;font-size:14px">';
flush();

const input = document.getElementById('num');
const r = input.getBoundingClientRect();
const x0 = (r.left + 10) | 0;
const y0 = (r.top + r.height / 2) | 0;

const moves = [];
document.addEventListener('mousemove', function (e) {
    moves.push({ x: e.clientX, target: e.target });
});

// The widget idiom, transcribed.
let value = 0;
let dragging = false;
let lastX = 0;
input.addEventListener('mousedown', function (e) {
    dragging = true;
    lastX = e.clientX;
});
document.addEventListener('mousemove', function (e) {
    if (!dragging) return;
    value += (e.clientX - lastX) / 10;
    lastX = e.clientX;
    input.value = value.toFixed(3);
});
document.addEventListener('mouseup', function () { dragging = false; });

mouseMove(x0, y0);
mouseDown(x0, y0);
const before = moves.length;
for (let i = 1; i <= 6; i++) mouseMove(x0 + i * 10, y0);
mouseUp(x0 + 60, y0);
flush();

const during = moves.length - before;
assert(during === 6,
       'every move between press and release reached the document, got ' + during + ' of 6');
assert(moves[moves.length - 1].target === input,
       'and targeted the element under the pointer');
assert(Math.abs(value - 6) < 0.001,
       'the scrub accumulated the whole drag, got ' + value + ' (expected 6)');
assert(input.value === '6.000', 'and wrote it back to the field, got ' + input.value);

// Moves after the release still flow (the drag state is gone, not the events).
const afterUp = moves.length;
mouseMove(x0 + 70, y0);
assert(moves.length === afterUp + 1, 'moves continue after mouseup');

// The other half: dragging inside the text still selects text. Give the field
// real content, press at the left edge and drag right.
input.value = 'abcdefghij';
flush();
click(x0, y0);                       // focus it
mouseDown((r.left + 4) | 0, y0);
for (let i = 1; i <= 6; i++) mouseMove((r.left + 4 + i * 12) | 0, y0);
mouseUp((r.left + 76) | 0, y0);
flush();
assert(input.selectionEnd > input.selectionStart,
       'the control still extends its own selection during the drag (start=' +
       input.selectionStart + ' end=' + input.selectionEnd + ')');

root.innerHTML = '';
console.log('PASS: a drag on a text input reaches the page as mousemove');
