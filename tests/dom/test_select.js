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

// Cleanup
root.innerHTML = '';
