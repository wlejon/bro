// A click on a <label> activates the control it labels.
//
// bro had no label activation behavior at all: clicking the TEXT of
// `<label><input type=checkbox> live</label>` did nothing, and neither did a
// `<label for=...>`. Every checkbox and radio in every app was therefore only
// operable by hitting its ~13px box exactly — the label, which is most of the
// clickable area and the part users aim at, was dead.
//
// The tricky half is not forwarding: it is NOT forwarding when the click already
// landed on the control. A naive implementation toggles a wrapped checkbox twice
// (once as the control's own activation, once via the label) and the checkbox
// appears dead in a different way.

const root = document.getElementById('root');

function setup(html) {
  root.innerHTML = html;
  flush();
}
function clickCenter(el) {
  const r = el.getBoundingClientRect();
  click(r.left + r.width / 2, r.top + r.height / 2);
  flush();
}
// Well inside the label but clear of a leading control's box.
function clickTextOf(el) {
  const r = el.getBoundingClientRect();
  click(r.left + r.width - 8, r.top + r.height / 2);
  flush();
}

// ── wrapping label ────────────────────────────────────────────────────────
setup('<label id="lab" style="position:absolute;left:0;top:0;width:200px;height:30px;">' +
      '<input id="cb" type="checkbox"> live</label>');
let cb = document.getElementById('cb');
let changes = 0;
cb.addEventListener('change', () => { changes++; });

clickTextOf(document.getElementById('lab'));
assert(cb.checked === true, 'label text click ticked the checkbox');
assert(changes === 1, 'exactly one change event (got ' + changes + ')');

clickTextOf(document.getElementById('lab'));
assert(cb.checked === false, 'a second label click unticks it');
assert(changes === 2, 'two change events total (got ' + changes + ')');

// The regression that a naive fix introduces: the control's own activation plus
// the label's would cancel out and the box would never change.
changes = 0;
clickCenter(cb);
assert(cb.checked === true, 'a direct click on the wrapped input still toggles');
assert(changes === 1, 'direct click fires change exactly once (got ' + changes + ')');

// ── label[for] pointing at a control elsewhere ────────────────────────────
setup('<label id="lab" for="far" style="position:absolute;left:0;top:0;width:200px;height:30px;">pick me</label>' +
      '<input id="far" type="checkbox" style="position:absolute;left:0;top:60px;width:20px;height:20px;">');
let far = document.getElementById('far');
let fchanges = 0;
far.addEventListener('change', () => { fchanges++; });
clickCenter(document.getElementById('lab'));
assert(far.checked === true, 'label[for] click ticked its target');
assert(fchanges === 1, 'label[for] fired one change (got ' + fchanges + ')');

// ── [for] wins, and does not fall back to a descendant ────────────────────
// A label with an explicit [for] that names nothing labelable labels NOTHING;
// silently ticking a nested input instead would activate a control the author
// did not point at.
setup('<label id="lab" for="nope" style="position:absolute;left:0;top:0;width:200px;height:30px;">' +
      '<input id="inner" type="checkbox"> text</label>' +
      '<div id="nope"></div>');
let inner = document.getElementById('inner');
clickTextOf(document.getElementById('lab'));
assert(inner.checked === false,
       '[for] naming a non-labelable element does not fall back to the descendant');

// ── radio groups go through the same activation ────────────────────────────
setup('<label id="l1" style="position:absolute;left:0;top:0;width:120px;height:24px;">' +
      '<input id="r1" type="radio" name="g" checked> one</label>' +
      '<label id="l2" style="position:absolute;left:0;top:30px;width:120px;height:24px;">' +
      '<input id="r2" type="radio" name="g"> two</label>');
clickTextOf(document.getElementById('l2'));
assert(document.getElementById('r2').checked === true, 'label click selected the second radio');
assert(document.getElementById('r1').checked === false, 'it deselected the first');

// ── interactive content inside a label handles its own click ───────────────
// A button beside the checkbox must not tick it — otherwise a "remove" button
// inside a label row does two things at once.
setup('<label id="lab" style="position:absolute;left:0;top:0;width:240px;height:30px;">' +
      '<input id="cb2" type="checkbox"> name ' +
      '<button id="btn" style="width:60px;height:20px;">x</button></label>');
let cb2 = document.getElementById('cb2');
let btnClicks = 0;
document.getElementById('btn').addEventListener('click', () => { btnClicks++; });
clickCenter(document.getElementById('btn'));
assert(btnClicks === 1, 'the button got its click (got ' + btnClicks + ')');
assert(cb2.checked === false, 'clicking a button inside the label did NOT tick the checkbox');

// ── a disabled control has no activation behavior ──────────────────────────
setup('<label id="lab" style="position:absolute;left:0;top:0;width:200px;height:30px;">' +
      '<input id="dis" type="checkbox" disabled> off limits</label>');
clickTextOf(document.getElementById('lab'));
assert(document.getElementById('dis').checked === false,
       'a label for a disabled control does nothing');

// ── preventDefault on the label's click suppresses forwarding ─────────────
setup('<label id="lab" style="position:absolute;left:0;top:0;width:200px;height:30px;">' +
      '<input id="cb3" type="checkbox"> cancel me</label>');
document.getElementById('lab').addEventListener('click', (e) => { e.preventDefault(); });
clickTextOf(document.getElementById('lab'));
assert(document.getElementById('cb3').checked === false,
       'preventDefault on the label suppresses activation');

// ── programmatic label.click() forwards too ───────────────────────────────
setup('<label id="lab"><input id="cb4" type="checkbox"> prog</label>');
let pchanges = 0;
document.getElementById('cb4').addEventListener('change', () => { pchanges++; });
document.getElementById('lab').click();
flush();
assert(document.getElementById('cb4').checked === true, 'label.click() ticked the checkbox');
assert(pchanges === 1, 'label.click() fired one change (got ' + pchanges + ')');

// ── a label wrapping no labelable control is inert ────────────────────────
setup('<label id="lab" style="position:absolute;left:0;top:0;width:200px;height:30px;">' +
      '<span>just words</span></label>');
clickCenter(document.getElementById('lab'));   // must not throw

// ── a nested span inside the label still forwards ─────────────────────────
// Labels in real UIs wrap their text in spans for styling; the click target is
// then the span, not the label.
setup('<label id="lab" style="position:absolute;left:0;top:0;width:200px;height:30px;">' +
      '<input id="cb5" type="checkbox"><span id="sp" style="display:inline-block;width:100px;">via span</span></label>');
let cb5changes = 0;
document.getElementById('cb5').addEventListener('change', () => { cb5changes++; });
clickCenter(document.getElementById('sp'));
assert(document.getElementById('cb5').checked === true, 'a click on a span inside the label forwards');
assert(cb5changes === 1, 'one change from the span click (got ' + cb5changes + ')');

root.innerHTML = '';
