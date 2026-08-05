// A cancelled click must undo the checkbox/radio toggle.
//
// The hit-tested mouse path applies the new checkedness on MOUSEDOWN
// (focusNewControl, replaced_elements.cpp) so the control ticks while the
// button is held. HTML makes that toggle part of the click's activation
// behaviour, so preventDefault() on the click has to put it back
// ("legacy-canceled-activation behavior"). Nothing did, so a cancelled click
// left the box ticked and the app's model and the DOM disagreed from then on.
//
// This is the real-mouse path specifically: element.click() never had the bug
// (it has no press half to undo) and is covered by test_click_checkbox.js.

const root = document.getElementById('root');
root.innerHTML = `
  <input id="a" type="checkbox" style="position:absolute;left:0;top:0;width:20px;height:20px">
  <input id="b" type="checkbox" checked style="position:absolute;left:0;top:40px;width:20px;height:20px">
  <input id="c" type="checkbox" style="position:absolute;left:0;top:80px;width:20px;height:20px">
  <input id="r1" type="radio" name="g" checked style="position:absolute;left:0;top:120px;width:20px;height:20px">
  <input id="r2" type="radio" name="g" style="position:absolute;left:0;top:160px;width:20px;height:20px">
`;
flush();

const a  = document.getElementById('a');
const b  = document.getElementById('b');
const c  = document.getElementById('c');
const r1 = document.getElementById('r1');
const r2 = document.getElementById('r2');

// ── control: an uncancelled click still toggles ──────────────────────────────
// Without this the test would pass just as well against a build that never
// toggled at all.
click(10, 10);
assert(a.checked === true, 'an ordinary click still checks the box');
click(10, 10);
assert(a.checked === false, 'and still unchecks it again');

// ── cancelled click on an unchecked box: stays unchecked ─────────────────────
a.addEventListener('click', (e) => e.preventDefault());
click(10, 10);
assert(a.checked === false,
    'cancelled click leaves an unchecked box unchecked');
click(10, 10);
assert(a.checked === false, 'and does so every time, not just the first');

// ── cancelled click on a checked box: stays checked ──────────────────────────
// The undo has to restore the value it found, not just clear the attribute.
assert(b.checked === true, 'b starts checked');
b.addEventListener('click', (e) => e.preventDefault());
click(10, 50);
assert(b.checked === true, 'cancelled click leaves a checked box checked');

// ── cancelling from a listener on an ANCESTOR counts too ─────────────────────
// preventDefault is a property of the event, not of where it was called.
root.addEventListener('click', (e) => {
    if (e.target === c) e.preventDefault();
});
click(10, 90);
assert(c.checked === false,
    'a click cancelled while bubbling undoes the toggle as well');

// ── radio: the undo has to give the check back to the group ──────────────────
// Unchecking r2 alone would leave the group with nothing selected, which is a
// state a radio group cannot reach by user interaction.
assert(r1.checked === true && r2.checked === false, 'radio group starts on r1');
r2.addEventListener('click', (e) => e.preventDefault());
click(10, 170);
assert(r2.checked === false, 'cancelled click does not check the clicked radio');
assert(r1.checked === true,
    'and hands the check back to the member that had it');

console.log('PASS');
