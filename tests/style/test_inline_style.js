// Test element.style (inline style) getters and setters

const root = document.getElementById('root');
const el = document.createElement('div');
root.appendChild(el);

// Set via style property
el.style.width = '100px';
assert(el.style.width === '100px', 'style.width getter');

el.style.backgroundColor = 'red';
assert(el.style.backgroundColor === 'red', 'style.backgroundColor getter');

// style.cssText
el.style.cssText = 'height: 50px; color: blue;';
assert(el.style.height === '50px', 'cssText sets height');
assert(el.style.color === 'blue', 'cssText sets color');

// setAttribute('style', ...) should work
el.setAttribute('style', 'width: 200px; margin: 10px;');
assert(el.style.width === '200px', 'setAttribute style sets width');
assert(el.style.margin === '10px', 'setAttribute style sets margin');

// CSSOM: setting a property to the empty string REMOVES it (so the cascade can
// fall back to an author stylesheet) — it must not linger as `display: ;`.
el.setAttribute('style', 'display: none; width: 10px;');
assert(el.style.display === 'none', 'inline display set');
el.style.display = '';
assert(el.style.display === '', 'display getter empty after clear');
assert(el.style.cssText.indexOf('display') === -1, 'display removed from cssText: ' + el.style.cssText);
assert(el.style.width === '10px', 'sibling property survives the clear');
// setProperty(name, '') is the same as the property setter '' — also removes
el.style.setProperty('width', '');
assert(el.style.width === '', 'setProperty empty removes width');
assert(el.style.cssText.indexOf('width') === -1, 'width removed from cssText: ' + el.style.cssText);
// a whitespace-only value counts as empty too
el.style.color = 'red';
el.style.color = '   ';
assert(el.style.color === '', 'whitespace-only value removes the property');

// Cleanup
root.innerHTML = '';
