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

// Cleanup
root.innerHTML = '';
