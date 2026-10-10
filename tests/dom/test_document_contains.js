// document.contains(node): true for a node connected to the document, false
// for a detached one (Node.contains on the document). UIs use it to check an
// element is still on the page, e.g. before showing its tooltip.

assert(typeof document.contains === 'function', 'document.contains exists');
assert(document.contains(document.body), 'the body is in the document');
assert(document.contains(document.documentElement), 'the root element is in the document');

const el = document.createElement('div');
assert(!document.contains(el), 'a created element is not in the document');
document.body.appendChild(el);
const inner = document.createElement('span');
el.appendChild(inner);
assert(document.contains(inner), 'a nested element is in the document');
el.remove();
assert(!document.contains(inner), 'a removed subtree is not in the document');
assert(!document.contains(null), 'null is not in the document');

console.log('test_document_contains PASSED');
