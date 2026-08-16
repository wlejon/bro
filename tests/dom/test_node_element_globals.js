// globalThis.Node and globalThis.Element, and the prototype chain under them.
//
// Both were broken, in two different ways, and the second way is the one worth
// having a test for.
//
// `Node` was not defined at all, so `x instanceof Node` — the ordinary way DOM
// code asks "is this something I can append?" — threw a ReferenceError instead
// of returning false. There was no correct way to write the test.
//
// `Element` WAS defined, by dom_polyfills.js, as `class Element {}` — an empty
// stub unrelated to the prototype elements actually have. So `el instanceof
// Element` was false for every element bro had ever created, silently, and a
// guard written that way took its else branch forever without anything going
// wrong loudly enough to notice.

const root = document.getElementById('root');

// ---------------------------------------------------------------------------
// the globals exist and refuse to be constructed
// ---------------------------------------------------------------------------

assert(typeof Node === 'function', 'Node is a global');
assert(typeof Element === 'function', 'Element is a global');
assert(typeof Node.prototype === 'object' && Node.prototype !== null,
       'Node.prototype is an object');
assert(typeof Element.prototype === 'object' && Element.prototype !== null,
       'Element.prototype is an object');

let threw = false;
try { new Node(); } catch (e) { threw = /Illegal constructor/.test(e.message); }
assert(threw, 'new Node() throws Illegal constructor');

threw = false;
try { new Element(); } catch (e) { threw = /Illegal constructor/.test(e.message); }
assert(threw, 'new Element() throws Illegal constructor');

// ---------------------------------------------------------------------------
// Element.prototype is the one elements actually use
// ---------------------------------------------------------------------------

const div = document.createElement('div');
// An element's own prototype is its per-tag interface (see
// test_html_interfaces.js); Element.prototype is further up the chain, and is
// still where every element method lives.
assert(Object.getPrototypeOf(div) === HTMLDivElement.prototype,
       'a created element starts at its own interface prototype');
assert(Element.prototype.isPrototypeOf(div),
       'Element.prototype is in the chain of a created element');
assert(typeof Element.prototype.querySelector === 'function',
       'Element.prototype carries the element methods');
assert(typeof div.querySelector === 'function',
       'and they resolve through the longer chain');

// ---------------------------------------------------------------------------
// instanceof
// ---------------------------------------------------------------------------

assert(div instanceof Element, 'an element is an Element');
assert(div instanceof Node, 'an element is a Node');
assert(document instanceof Node, 'the document is a Node');
assert(!(document instanceof Element), 'the document is not an Element');

// An element in the live tree behaves the same as a detached one.
root.appendChild(div);
flush();
assert(div instanceof Element, 'an attached element is still an Element');
assert(div instanceof Node, 'an attached element is still a Node');

// Non-nodes must return false rather than throw — this is the whole point of
// having the global, since the test is normally written as a guard.
assert(!('x' instanceof Node), 'a string is not a Node');
assert(!(({}) instanceof Node), 'a plain object is not a Node');
assert(!((42) instanceof Element), 'a number is not an Element');
assert(!(null instanceof Node) === true, 'null is not a Node');

// The guard this all exists for.
function accepts(v) { return v instanceof Node ? 'node' : 'text'; }
assert(accepts(div) === 'node', 'the append guard takes the node branch');
assert(accepts('hello') === 'text', 'the append guard takes the text branch');

// ---------------------------------------------------------------------------
// the prototype chain
// ---------------------------------------------------------------------------

const chain = [];
let p = Object.getPrototypeOf(div);
while (p) { chain.push(p); p = Object.getPrototypeOf(p); }
assert(chain[0] === HTMLDivElement.prototype, 'chain starts at the tag interface');
assert(chain[1] === HTMLElement.prototype, 'HTMLElement.prototype is above that');
assert(chain[2] === Element.prototype, 'Element.prototype is above that');
assert(chain[3] === Node.prototype, 'Node.prototype is directly above Element.prototype');
assert(chain[4] === Object.prototype, 'Object.prototype tops it off');
assert(chain.length === 5, `chain is exactly 5 deep, got ${chain.length}`);

// ---------------------------------------------------------------------------
// nodeType constants
// ---------------------------------------------------------------------------

// The spec puts these on both the constructor and the prototype, and real code
// uses both spellings.
assert(Node.ELEMENT_NODE === 1, 'Node.ELEMENT_NODE');
assert(Node.ATTRIBUTE_NODE === 2, 'Node.ATTRIBUTE_NODE');
assert(Node.TEXT_NODE === 3, 'Node.TEXT_NODE');
assert(Node.COMMENT_NODE === 8, 'Node.COMMENT_NODE');
assert(Node.DOCUMENT_NODE === 9, 'Node.DOCUMENT_NODE');
assert(Node.DOCUMENT_FRAGMENT_NODE === 11, 'Node.DOCUMENT_FRAGMENT_NODE');
assert(Node.prototype.TEXT_NODE === 3, 'Node.prototype.TEXT_NODE');
assert(div.ELEMENT_NODE === 1, 'an element inherits the constants');

assert(div.nodeType === Node.ELEMENT_NODE,
       `an element's nodeType is ELEMENT_NODE, got ${div.nodeType}`);
assert(document.nodeType === Node.DOCUMENT_NODE,
       `the document's nodeType is DOCUMENT_NODE, got ${document.nodeType}`);

// ---------------------------------------------------------------------------
// text and comment nodes go through the same class
// ---------------------------------------------------------------------------

div.innerHTML = 'some text<!-- a comment -->';
flush();
const kids = div.childNodes;
assert(kids.length >= 1, 'the element has child nodes');
for (let i = 0; i < kids.length; i++) {
    const n = kids[i];
    assert(n instanceof Node, `child ${i} (nodeType ${n.nodeType}) is a Node`);
    assert(!(n instanceof Element),
           `child ${i} (nodeType ${n.nodeType}) is not an Element`);
    assert(n.nodeType === Node.TEXT_NODE || n.nodeType === Node.COMMENT_NODE,
           `child ${i} is text or comment, got nodeType ${n.nodeType}`);
}

console.log('OK — Node and Element globals, prototype chain, nodeType constants');
