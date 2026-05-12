// Test document-level factory and query methods — exercises
// src/js/document_bindings.cpp paths beyond createElement that
// existing tests already cover (importNode, adoptNode,
// getElementsByTagName/ClassName/Name, createDocumentFragment,
// createElementNS, activeElement).

const root = document.getElementById('root');
root.innerHTML =
    '<div id="a" class="foo bar"><span id="s1" class="bar">s1</span></div>' +
    '<div id="b" class="foo" data-x="1"></div>' +
    '<input id="i" name="user" type="text">' +
    '<input name="user" type="text">';
flush();

// =========================================================================
// getElementsByTagName (live collection)
// =========================================================================
const divs = document.getElementsByTagName('div');
assert(typeof divs.length === 'number', 'tag collection has length');
assert(divs.length >= 2, 'at least 2 divs, got ' + divs.length);
assert(divs[0].tagName === 'DIV', 'first is DIV');

const inputs = document.getElementsByTagName('input');
assert(inputs.length >= 2, '2 inputs');

// Live: adding a tag updates the collection
const beforeLen = divs.length;
const newDiv = document.createElement('div');
root.appendChild(newDiv);
const afterLen = divs.length;
assert(afterLen === beforeLen + 1, 'live collection updates, before=' + beforeLen + ' after=' + afterLen);
root.removeChild(newDiv);

// =========================================================================
// getElementsByClassName
// =========================================================================
const foos = document.getElementsByClassName('foo');
assert(foos.length === 2, 'foo class has 2 elements');

const bars = document.getElementsByClassName('bar');
assert(bars.length === 2, 'bar class has 2 elements');

// =========================================================================
// getElementsByName
// =========================================================================
const users = document.getElementsByName('user');
assert(users.length === 2, 'name=user has 2 elements');

// =========================================================================
// createDocumentFragment
// =========================================================================
const frag = document.createDocumentFragment();
assert(frag !== null, 'fragment created');
// nodeType = 11
assert(frag.nodeType === 11, 'fragment nodeType = 11, got ' + frag.nodeType);

// Append into fragment, then move into DOM
const f1 = document.createElement('span'); f1.id = 'f1';
const f2 = document.createElement('span'); f2.id = 'f2';
frag.appendChild(f1);
frag.appendChild(f2);

root.appendChild(frag);
flush();
assert(document.getElementById('f1') !== null, 'fragment child f1 in DOM');
assert(document.getElementById('f2') !== null, 'fragment child f2 in DOM');

// =========================================================================
// createElementNS (SVG-like) — namespace ignored but element created
// =========================================================================
const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
assert(svg !== null, 'createElementNS returns element');
// tagName should reflect the local name
assert(svg.tagName.toLowerCase() === 'svg', 'NS element has tagName, got ' + svg.tagName);

// =========================================================================
// activeElement
// =========================================================================
// In headless without focus, activeElement may be body or null
const ae = document.activeElement;
assert(ae === null || ae === document.body || ae.tagName !== undefined,
       'activeElement is null, body, or element');

// Focus an input — activeElement reflects it
const input = document.getElementById('i');
input.focus();
assert(document.activeElement === input, 'activeElement updated after focus');

// =========================================================================
// importNode — clones a node tree
// =========================================================================
const template = document.createElement('div');
template.id = 'tmpl';
template.innerHTML = '<p>imported</p>';
// importNode(node, deep)
const imp = document.importNode(template, true);
assert(imp !== null, 'importNode returns a node');
assert(imp.tagName.toLowerCase() === 'div', 'imported is a div');
// Deep clone copies children
assert(imp.children && imp.children.length === 1, 'deep import copies children');

const imp2 = document.importNode(template, false);
assert(imp2 !== null, 'shallow importNode returns');
// Shallow: no children
const sc = imp2.children ? imp2.children.length : 0;
assert(sc === 0, 'shallow import has no children, got ' + sc);

// =========================================================================
// adoptNode — moves node to current document (no-op in single-doc)
// =========================================================================
const ad = document.createElement('em');
const adopted = document.adoptNode(ad);
assert(adopted !== null, 'adoptNode returns the node');

// =========================================================================
// querySelectorAll returns NodeList with length
// =========================================================================
const qall = document.querySelectorAll('div.foo');
assert(qall.length === 2, 'querySelectorAll(div.foo) finds 2');

const noMatch = document.querySelectorAll('div.zzz');
assert(noMatch.length === 0, 'no match returns empty');

// =========================================================================
// Cleanup
// =========================================================================
root.innerHTML = '';
