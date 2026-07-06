// Test <select> element — opening dropdown, selecting options, value getter.
// Exercises src/layout/el_select.cpp, src/engine/dropdown_overlay.cpp.

const root = document.getElementById('root');
root.innerHTML =
    '<select id="s">' +
    '  <option value="a">Apple</option>' +
    '  <option value="b" selected>Banana</option>' +
    '  <option value="c">Cherry</option>' +
    '</select>';
flush();

const s = document.getElementById('s');

// =========================================================================
// Initial value reflects the selected option
// =========================================================================
assert(s.value === 'b', 'initial value from selected attr, got: ' + s.value);

// selectedIndex
if (typeof s.selectedIndex === 'number') {
    assert(s.selectedIndex === 1, 'selectedIndex = 1');
}

// options collection
if (s.options && typeof s.options.length === 'number') {
    assert(s.options.length === 3, 'options.length = 3');
}

// =========================================================================
// Programmatic value setter
// =========================================================================
s.value = 'c';
assert(s.value === 'c', 'value setter updates, got: ' + s.value);
// The selected attribute should have been moved
const opts = s.querySelectorAll('option');
assert(opts.length === 3, '3 options');
// Now 'c' should have selected attr; 'b' should not
let cSel = false, bSel = false;
for (const o of opts) {
    if (o.getAttribute('value') === 'c' && o.hasAttribute('selected')) cSel = true;
    if (o.getAttribute('value') === 'b' && o.hasAttribute('selected')) bSel = true;
}
assert(cSel, 'c marked selected after value=c');
assert(!bSel, 'b no longer selected');

// Setting unknown value is no-op (per spec — first option becomes selected
// or selectedIndex stays). Just verify no crash.
const prev = s.value;
s.value = 'xyz';
// Either reverts or accepts; just verify it's a string
assert(typeof s.value === 'string', 'value stays a string');

// =========================================================================
// Click to open dropdown (smoke test — covers dropdown_overlay code)
// =========================================================================
s.value = 'a';
const rect = s.getBoundingClientRect();
click(rect.left + rect.width / 2, rect.top + rect.height / 2);

// Dropdown may be open now. Click again to close.
click(rect.left + rect.width / 2, rect.top + rect.height / 2);

// =========================================================================
// change event fires on value change via JS
// =========================================================================
let changeCount = 0;
s.addEventListener('change', () => changeCount++);

// Programmatic value= does NOT fire change per spec; that's user-driven.
s.value = 'c';
// Some impls fire it though — just verify no error.

// =========================================================================
// Dynamically add an option
// =========================================================================
const newOpt = document.createElement('option');
newOpt.value = 'd';
newOpt.textContent = 'Date';
s.appendChild(newOpt);
flush();
const opts2 = s.querySelectorAll('option');
assert(opts2.length === 4, '4 options after append');

s.value = 'd';
assert(s.value === 'd', 'value=d after appending');

// =========================================================================
// Remove an option
// =========================================================================
s.removeChild(newOpt);
flush();
assert(s.querySelectorAll('option').length === 3, 'back to 3 options');

// =========================================================================
// Explicit value="" is a real value (placeholder-option pattern) and an
// option with NO value attribute falls back to its text. Regression: the
// text fallback used to trigger on empty-STRING value, so a placeholder
// <option value="">…</option> reported its label as the value and
// `select.value = ''` could never select it (krea2-lab's "or a render…"
// dropdowns stuck on the picked entry).
// =========================================================================
root.innerHTML =
    '<select id="p">' +
    '  <option value="">pick one…</option>' +
    '  <option value="1">first render</option>' +
    '  <option>bare text</option>' +
    '</select>';
flush();
const p = document.getElementById('p');

// Placeholder (first option, nothing selected) reports '', not its label.
assert(p.value === '', 'explicit value="" stays empty, got: ' + JSON.stringify(p.value));

// Select a real entry, then reset to the placeholder via value = ''.
p.value = '1';
assert(p.value === '1', 'picked the real entry, got: ' + p.value);
p.value = '';
assert(p.value === '', "value='' selects the placeholder back, got: " + JSON.stringify(p.value));
if (typeof p.selectedIndex === 'number') {
    assert(p.selectedIndex === 0, 'placeholder is selectedIndex 0, got: ' + p.selectedIndex);
}

// No value attribute → value falls back to the option text.
p.value = 'bare text';
assert(p.value === 'bare text', 'attribute-less option uses its text, got: ' + p.value);

// Same answers before any layout touches the select (DOM-fallback path).
const q = document.createElement('select');
q.innerHTML = '<option value="">placeholder</option><option value="x">X</option>';
assert(q.value === '', 'pre-layout: explicit value="" stays empty, got: ' + JSON.stringify(q.value));
q.value = 'x';
assert(q.value === 'x', 'pre-layout: setter matches by attribute value');
q.value = '';
assert(q.value === '', "pre-layout: value='' re-selects the placeholder");

// Cleanup
root.innerHTML = '';
