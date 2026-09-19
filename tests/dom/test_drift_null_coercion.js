// WebIDL null handling on the string-valued DOM members.
//
// `textContent` is `DOMString?` and `innerHTML`/`CharacterData.data` are
// `[LegacyNullToEmptyString] DOMString`, so assigning null to any of them
// CLEARS the node. `id`, `className` and `setAttribute` are plain `DOMString`,
// so null there is the four-character string "null" — the difference is the
// whole point of the annotation, and code that clears a label with
// `el.textContent = obj.label` (label absent, so null) must not end up
// printing "null" at the user.
//
// Regression: the bronze port guarded these with `if (!isObject(v))` and then
// called toUtf8, and `isObject(null)` is false in bronze — so null fell
// through the guard and stringified.

const root = document.getElementById('root');

const div = document.createElement('div');
root.appendChild(div);

// --- textContent = null clears -------------------------------------------
div.textContent = 'before';
assert(div.textContent === 'before', 'textContent set to a string');
div.textContent = null;
assert(div.textContent === '', 'textContent = null clears, it does not write "null"');
assert(div.childNodes.length === 0, 'textContent = null leaves no text node');

div.textContent = 'again';
div.textContent = undefined;
assert(div.textContent === '', 'textContent = undefined clears too');

// --- innerHTML = null clears ---------------------------------------------
div.innerHTML = '<span>x</span>';
assert(div.children.length === 1, 'innerHTML set to markup');
div.innerHTML = null;
assert(div.innerHTML === '', 'innerHTML = null clears');
assert(div.children.length === 0, 'innerHTML = null removes children');

// --- CharacterData.data / nodeValue / textContent = null clears -----------
const text = document.createTextNode('payload');
div.appendChild(text);
text.data = null;
assert(text.data === '', 'text.data = null clears');
text.data = 'payload';
text.nodeValue = null;
assert(text.nodeValue === '', 'text.nodeValue = null clears');
text.data = 'payload';
text.textContent = null;
assert(text.textContent === '', 'text.textContent = null clears');

// --- the plain-DOMString members keep stringifying ------------------------
div.id = null;
assert(div.id === 'null', 'id is a plain DOMString: null stringifies');
div.id = 'drift-null';
div.className = null;
assert(div.className === 'null', 'className is a plain DOMString: null stringifies');
div.className = '';
div.setAttribute('data-x', null);
assert(div.getAttribute('data-x') === 'null',
       'setAttribute is a plain DOMString: null stringifies');

// --- innerText writes go through the same reader --------------------------
div.innerText = 'shown';
assert(div.innerText === 'shown', 'innerText round-trips');
div.innerText = null;
assert(div.innerText === '', 'innerText = null clears');

root.innerHTML = '';
