// A function of an <iframe>'s realm, called by the parent through
// iframe.contentWindow, runs against its own realm's document and globals,
// as it does in a browser. Sub-document realms share one global object and
// are entered by swapping it (bronze_host: enterRealmScope, the document
// global, the current document); a call from the parent entered nothing, so
// the child's function ran against the parent's document: its
// document.querySelectorAll found the parent's elements, its createElement
// went into the parent. A function the proxy hands out now enters the
// child's realm around the call, and leaves it after, whatever it returns or
// throws.

document.title = 'Parent title';
var secret = 'parent';
const frame = document.createElement('iframe');
frame.setAttribute('src', 'realm_child');
frame.style.cssText = 'width: 200px; height: 100px';
document.body.appendChild(frame);
flush();
const cw = frame.contentWindow;
assert(cw && typeof cw.titleOf === 'function', 'the child loaded and its functions are on its contentWindow');

assert(cw.titleOf() === 'Realm child', `the child's function reads the child's document (${cw.titleOf()})`);
assert(cw.countBoxes() === 2, `its querySelectorAll runs on the child's document (${cw.countBoxes()})`);
const before = document.querySelectorAll('.box').length;
assert(cw.addBox() === 3, 'its createElement and appendChild go into the child');
assert(document.querySelectorAll('.box').length === before, 'and not into the parent');
assert(cw.document.querySelectorAll('.box').length === 3, 'the child document has the box');
assert(cw.readSecret() === 'child', `its globals are the child realm's (${cw.readSecret()})`);
assert(cw.sum(2, 3) === 5, 'arguments and the result pass through');

// After each call the parent's realm is back.
assert(document.title === 'Parent title', 'the parent document is the global document again');
assert(secret === 'parent', 'and the parent realm\'s globals');

// A throw in the child reaches the caller, and the parent's realm is back.
let caught = null;
try { cw.throwIt(); } catch (e) { caught = e; }
assert(caught && String(caught.message).includes('thrown in the child'), `the child's throw reaches the caller (${caught})`);
assert(document.title === 'Parent title' && secret === 'parent', 'the parent realm is back after a throw');

// The same function each time it is read, and one it returns runs in the child too.
assert(cw.titleOf === cw.titleOf, 'a function read twice is the same function');
const fn = cw.counter();
assert(typeof fn === 'function' && fn() === 3, 'a function the child returns runs against the child document');
console.log('iframe realm call: the child\'s functions ran in the child');
