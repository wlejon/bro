// element.click() runs the ACTIVATION BEHAVIOUR, not just a click event.
//
// The hit-tested path (a real mouse press) has always done this; the
// programmatic one is the same behaviour reached from script, and libraries
// reach it constantly — a "Save" button driven by a keyboard shortcut, a
// hidden <input type=file> opened from a styled button, a details/summary
// disclosure toggled from a menu.
//
// Regression: the bronze port's click() dispatched a MouseEvent and stopped.
// No focus, no form submit, no reset, no checkbox toggle, no summary toggle,
// no label forwarding, and no `disabled` gate — a disabled button's listeners
// still ran.

const root = document.getElementById('root');

function setup(html) {
    root.innerHTML = html;
    flush();
}

// --- focus ---------------------------------------------------------------
setup('<input id="i" type="text">');
document.getElementById('i').click();
assert(document.activeElement === document.getElementById('i'),
       'click() focuses the element it activates');

// --- checkbox toggles and fires change ------------------------------------
setup('<input id="cb" type="checkbox">');
const cb = document.getElementById('cb');
let changes = 0;
cb.addEventListener('change', function () { changes++; });
cb.click();
assert(cb.checked === true, 'click() ticks a checkbox');
assert(changes === 1, 'click() on a checkbox fires change');
cb.click();
assert(cb.checked === false, 'a second click() unticks it');
assert(changes === 2, 'the untick fires change too');

// --- radio: one click clears the rest of the group ------------------------
setup('<input id="r1" type="radio" name="g" checked>' +
      '<input id="r2" type="radio" name="g">');
document.getElementById('r2').click();
assert(document.getElementById('r2').checked === true, 'click() checks the radio');
assert(document.getElementById('r1').checked === false,
       'checking a radio clears its group');

// --- submit button submits its form ---------------------------------------
setup('<form id="f"><input name="a" value="x">' +
      '<button id="go" type="submit">go</button></form>');
let submits = 0, submitter = null;
document.getElementById('f').addEventListener('submit', function (e) {
    submits++;
    submitter = e.submitter;
    e.preventDefault();
});
document.getElementById('go').click();
assert(submits === 1, 'click() on a submit button submits the form');
assert(submitter === document.getElementById('go'),
       'the submit event names the submitter');

// A <button> with no type attribute is a submit button.
setup('<form id="f2"><button id="bare">go</button></form>');
let submits2 = 0;
document.getElementById('f2').addEventListener('submit', function (e) {
    submits2++; e.preventDefault();
});
document.getElementById('bare').click();
assert(submits2 === 1, 'a type-less <button> is a submit button');

// --- reset button fires reset ---------------------------------------------
setup('<form id="f3"><select id="s"><option>a</option>' +
      '<option selected>b</option></select>' +
      '<button id="rb" type="reset">reset</button></form>');
const sel = document.getElementById('s');
let resets = 0;
document.getElementById('f3').addEventListener('reset', function () { resets++; });
sel.selectedIndex = 0;
document.getElementById('rb').click();
assert(resets === 1, 'click() on a reset button fires reset');
assert(sel.selectedIndex === 1, 'reset puts the select back on its selected option');

// A type="button" does neither.
setup('<form id="f4"><button id="plain" type="button">x</button></form>');
let submits4 = 0;
document.getElementById('f4').addEventListener('submit', function (e) {
    submits4++; e.preventDefault();
});
document.getElementById('plain').click();
assert(submits4 === 0, 'type="button" does not submit');

// --- summary toggles its details ------------------------------------------
setup('<details id="d"><summary id="sm">more</summary><p>body</p></details>');
const det = document.getElementById('d');
let toggles = 0;
det.addEventListener('toggle', function () { toggles++; });
assert(det.hasAttribute('open') === false, 'the details starts closed');
document.getElementById('sm').click();
assert(det.hasAttribute('open') === true, 'click() on the summary opens the details');
assert(toggles === 1, 'opening fired toggle');
document.getElementById('sm').click();
assert(det.hasAttribute('open') === false, 'a second click() closes it');

// --- label forwarding, and the disabled gate ------------------------------
setup('<label id="lb" for="tgt">tick</label><input id="tgt" type="checkbox">');
document.getElementById('lb').click();
assert(document.getElementById('tgt').checked === true,
       'label.click() forwards to the control it labels');

// A wrapped control must not be toggled twice.
setup('<label id="lw"><input id="in" type="checkbox"> live</label>');
document.getElementById('in').click();
assert(document.getElementById('in').checked === true,
       'clicking the wrapped control toggles it exactly once');

// A disabled control has no activation behaviour — not even the click event.
setup('<input id="dis" type="checkbox" disabled>');
const dis = document.getElementById('dis');
let clicksOnDisabled = 0;
dis.addEventListener('click', function () { clicksOnDisabled++; });
dis.click();
assert(dis.checked === false, 'a disabled checkbox does not toggle');
assert(clicksOnDisabled === 0, 'a disabled control gets no click event either');

// The same, one level up: a control inside a disabled <fieldset>.
setup('<fieldset disabled><input id="fs" type="checkbox"></fieldset>');
document.getElementById('fs').click();
assert(document.getElementById('fs').checked === false,
       'a control in a disabled fieldset does not activate');

// A label forwarding into a disabled fieldset must not activate it either.
setup('<fieldset disabled><label id="lfs" for="fc">x</label>' +
      '<input id="fc" type="checkbox"></fieldset>');
document.getElementById('lfs').click();
assert(document.getElementById('fc').checked === false,
       'label forwarding stops at a disabled fieldset');

// --- preventDefault on the click cancels the activation -------------------
setup('<input id="pv" type="checkbox">');
const pv = document.getElementById('pv');
pv.addEventListener('click', function (e) { e.preventDefault(); });
pv.click();
assert(pv.checked === false, 'preventDefault on the click cancels the toggle');

root.innerHTML = '';
