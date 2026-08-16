// Overlay anchoring for the replaced controls that open one: <input type=color>
// and <select>. The anchor is resolved from live layout, not from the last
// paint — a control can be revealed and clicked in the same turn (routine when
// a script drives the UI), and a paint-time anchor is {0,0,0,0} then, so the
// popup opened in the window corner and the next click, aimed at the control,
// counted as "outside" and dismissed it.

const root = document.getElementById('root');

// ---- Colour picker --------------------------------------------------------
// No screenshot()/getPixel() anywhere in this test, so nothing here is ever
// painted before it is clicked.
root.innerHTML = '<div style="padding:40px 60px">' +
                 '<input id="sw" type="color" value="#ff0000" style="width:40px;height:20px"></div>';
flush();

const sw = document.getElementById('sw');
const r = sw.getBoundingClientRect();
assert(r.width > 0 && r.top > 0, 'swatch has a box away from the origin');

let inputs = 0;
sw.addEventListener('input', () => inputs++);

click(r.left + r.width / 2, r.top + r.height / 2);
advanceTime(50); flush();

// The picker opens directly below the swatch; this lands in its SV square.
const px = r.left + 40, py = r.top + r.height + 60;
mouseDown(px, py);
mouseUp(px, py);
advanceTime(50); flush();

assert(inputs > 0, 'a click inside the picker reaches it (input fired)');
assert(sw.value !== '#ff0000', 'the picked colour was applied: ' + sw.value);

// Dismiss it: a click outside commits and closes.
click(r.left + 600, r.top + 400);
advanceTime(50); flush();

// ---- Select dropdown ------------------------------------------------------
root.innerHTML = '<div style="padding:200px 300px"><select id="s">' +
                 '<option value="a">Alpha</option><option value="b">Beta</option>' +
                 '<option value="c">Gamma</option></select></div>';
flush();

const s = document.getElementById('s');
const sr = s.getBoundingClientRect();
assert(sr.width > 0 && sr.top > 0, 'select has a box away from the origin');

let changes = 0;
s.addEventListener('change', () => changes++);

click(sr.left + sr.width / 2, sr.top + sr.height / 2);
advanceTime(50); flush();

// Second row of the open list, just under the control.
click(sr.left + 10, sr.top + sr.height + 28);
advanceTime(50); flush();

assert(s.value === 'b', 'the option under the pointer was picked: ' + s.value);
assert(changes === 1, 'change fired once');

root.innerHTML = '';
