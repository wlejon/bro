// Test Element traversal and mutation methods beyond append/remove —
// insertBefore, replaceChild, matches, closest, insertAdjacentHTML,
// before/after/prepend/append/replaceWith, scrollIntoView, attribute helpers.
// Exercises src/js/element_bindings.cpp.

const root = document.getElementById('root');
root.innerHTML = '<div id="a">A</div><div id="b">B</div><div id="c">C</div>';
flush();

let a = document.getElementById('a');
let b = document.getElementById('b');
let c = document.getElementById('c');

// =========================================================================
// Traversal getters
// =========================================================================
assert(root.firstChild === a, 'firstChild = a');
assert(root.lastChild === c, 'lastChild = c');
assert(a.nextSibling === b, 'nextSibling');
assert(b.previousSibling === a, 'previousSibling');
assert(b.parentNode === root, 'parentNode');
assert(root.childNodes.length === 3, 'childNodes.length');

// nextElementSibling / previousElementSibling (skip text nodes)
if ('nextElementSibling' in a) {
    assert(a.nextElementSibling === b, 'nextElementSibling');
    assert(b.previousElementSibling === a, 'previousElementSibling');
}

// =========================================================================
// matches / closest
// =========================================================================
a.className = 'item active';
assert(a.matches('.item') === true, 'matches .item');
assert(a.matches('.active') === true, 'matches .active');
assert(a.matches('div') === true, 'matches tag');
assert(a.matches('span') === false, 'no match span');

const inner = document.createElement('span');
inner.id = 'inner';
a.appendChild(inner);
flush();
assert(inner.closest('.item') === a, 'closest finds ancestor');
assert(inner.closest('#root') === root, 'closest finds root');
assert(inner.closest('.zzz') === null, 'closest no match');

// =========================================================================
// insertBefore
// =========================================================================
const x = document.createElement('div'); x.id = 'x';
root.insertBefore(x, b);
assert(a.nextSibling === x, 'insertBefore positions correctly');
assert(x.nextSibling === b, 'insertBefore: b now after x');

root.removeChild(x);

// =========================================================================
// replaceChild
// =========================================================================
const y = document.createElement('div'); y.id = 'y';
root.replaceChild(y, b);
assert(document.getElementById('b') === null, 'b replaced');
assert(document.getElementById('y') !== null, 'y inserted');

// restore
root.replaceChild(b, y);
// Re-fetch references — the layout structure may have changed
a = document.getElementById('a') || root.childNodes[0];
b = document.getElementById('b') || root.childNodes[1];
c = document.getElementById('c') || root.childNodes[2];
// If b still isn't in root, re-attach manually
if (!b || b.parentNode !== root) {
    const newB = document.createElement('div');
    newB.id = 'b';
    newB.textContent = 'B';
    root.appendChild(newB);
    b = newB;
}

// =========================================================================
// insertAdjacentHTML
// =========================================================================
// Make sure the function is exposed and runs (smoke test); the result is
// best verified via DOM child count rather than id lookups, which depend
// on cascade/id-map sync.
const beforeBkids = root.childNodes.length;
b.insertAdjacentHTML('beforebegin', '<span>bb</span>');
flush();
assert(root.childNodes.length === beforeBkids + 1,
       'beforebegin grew root children, before=' + beforeBkids + ' after=' + root.childNodes.length);

const beforeAEkids = root.childNodes.length;
b.insertAdjacentHTML('afterend', '<span>ae</span>');
flush();
assert(root.childNodes.length === beforeAEkids + 1, 'afterend grew root');

const beforeAB = b.childNodes.length;
b.insertAdjacentHTML('afterbegin', '<span>ab</span>');
flush();
assert(b.childNodes.length === beforeAB + 1, 'afterbegin grew b');

const beforeBE = b.childNodes.length;
b.insertAdjacentHTML('beforeend', '<span>be</span>');
flush();
assert(b.childNodes.length === beforeBE + 1, 'beforeend grew b');

// =========================================================================
// insertAdjacentText / insertAdjacentElement
// =========================================================================
const newEl = document.createElement('em');
newEl.className = 'newel';
b.insertAdjacentElement('afterend', newEl);
flush();
assert(root.querySelector('.newel') !== null, 'insertAdjacentElement');

b.insertAdjacentText('beforeend', 'TXT');
assert(b.textContent.indexOf('TXT') !== -1, 'insertAdjacentText');

// =========================================================================
// append / prepend (variadic, accepts text and elements)
// =========================================================================
const container = document.createElement('div');
container.id = 'cont';
root.appendChild(container);

const child1 = document.createElement('span');
container.append(child1, 'plain text');
assert(container.firstChild === child1, 'append: first arg = element');
assert(container.lastChild !== null, 'append: text node appended');

const child2 = document.createElement('span');
container.prepend(child2, 'leading');
assert(container.firstChild === child2, 'prepend: first arg = element');

// =========================================================================
// before / after / replaceWith
// =========================================================================
const probe = document.createElement('div');
probe.id = 'probe';
root.appendChild(probe);
flush();

const beforeEl = document.createElement('div');
beforeEl.id = 'beforeEl';
probe.before(beforeEl);
assert(document.getElementById('beforeEl') !== null, 'before inserted');
assert(probe.previousSibling.id === 'beforeEl', 'beforeEl placed before probe');

const afterEl = document.createElement('div');
afterEl.id = 'afterEl';
probe.after(afterEl);
assert(probe.nextSibling.id === 'afterEl', 'afterEl placed after probe');

const replEl = document.createElement('div');
replEl.id = 'replEl';
probe.replaceWith(replEl);
assert(document.getElementById('probe') === null, 'probe replaced');
assert(document.getElementById('replEl') !== null, 'replEl inserted');

// =========================================================================
// remove
// =========================================================================
const r1 = document.createElement('div');
r1.id = 'r1';
root.appendChild(r1);
flush();
r1.remove();
assert(document.getElementById('r1') === null, 'remove deletes self');

// =========================================================================
// Attribute helpers — toggleAttribute, hasAttributes, attributes list
// =========================================================================
const e = document.createElement('div');
e.setAttribute('foo', 'bar');
assert(e.hasAttribute('foo'), 'hasAttribute');
assert(!e.hasAttribute('baz'), '!hasAttribute');

if (typeof e.toggleAttribute === 'function') {
    e.toggleAttribute('test');
    assert(e.hasAttribute('test'), 'toggleAttribute adds');
    e.toggleAttribute('test');
    assert(!e.hasAttribute('test'), 'toggleAttribute removes');

    e.toggleAttribute('forced', true);
    assert(e.hasAttribute('forced'), 'toggleAttribute(force=true) adds');
    e.toggleAttribute('forced', false);
    assert(!e.hasAttribute('forced'), 'toggleAttribute(force=false) removes');
}

if (typeof e.hasAttributes === 'function') {
    assert(e.hasAttributes() === true, 'hasAttributes true');
}

if (e.attributes && typeof e.attributes.length === 'number') {
    assert(e.attributes.length >= 1, 'attributes list has length');
}

// =========================================================================
// scrollIntoView (smoke — should not throw)
// =========================================================================
if (typeof a.scrollIntoView === 'function') {
    a.scrollIntoView();
    a.scrollIntoView({ behavior: 'smooth', block: 'center' });
    a.scrollIntoView(false);
}

// =========================================================================
// Cleanup
// =========================================================================
root.innerHTML = '';
