// The node probe: text nodes, comments, fragments, and the tree surface they
// share with elements.
//
// dom_probe.js is about the DOM an app BUILDS — createElement, appendChild,
// read it back. This one is about the DOM an app is HANDED, and about the three
// node kinds that are not elements. They are a different subject because they
// fail differently: the element surface breaks by losing identity or handing
// back a fake array, and this one breaks by quietly SKIPPING nodes. An
// elements-only childNodes does not throw, does not log, and does not look
// wrong — it just reports three where the document holds seven, and every
// caller that trusted it walks past the text it was looking for.
//
// So most lines below print a pair: the node-level answer and the
// element-level one, for the same question on the same element. `#mixed` in
// this appdir's index.html is authored so those two answers MUST differ —
// four child nodes, one child element — and a regression that collapses the
// node view back onto the element view shows up as two numbers that became
// equal, which no amount of squinting at one of them would have caught.
//
// Three things here are not obvious enough to leave unexplained.
//
// OFFSETS ARE UTF-16. Every index in the CharacterData API is a JS string
// index, and JS strings are UTF-16 while the DOM stores UTF-8. The `u16`
// section uses a two-byte character and a four-byte one on purpose: an
// implementation that forwards the offset straight to a byte-indexed std::string
// gets ASCII exactly right and cuts those in half.
//
// A FRAGMENT DISAPPEARS. Appending one must insert its CHILDREN and leave the
// fragment empty. The probe checks the parent's gain and the fragment's loss in
// the same breath, because an implementation that inserts the fragment itself
// satisfies the first half.
//
// CLONES ARE NEW OBJECTS. `clone !== original` is the assertion that a clone
// went through the document's clone algorithm rather than being handed back the
// same registry entry.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// ---------------------------------------------------------------------------
// A text node exists, and is a node
// ---------------------------------------------------------------------------

const t = document.createTextNode('hello');
say('text.nodeType', t.nodeType);
say('text.nodeName', t.nodeName);
say('text.data', t.data);
// data / nodeValue / textContent are three names for one string.
say('text.nodeValue', t.nodeValue === 'hello');
say('text.textContent', t.textContent === 'hello');
say('text.length', t.length);
say('text.parentNull', t.parentNode === null);

t.data = 'goodbye';
say('text.writeData', t.data);
t.nodeValue = 'hello';
say('text.writeNodeValue', t.data);

// ---------------------------------------------------------------------------
// It goes into the tree, and the tree admits it
// ---------------------------------------------------------------------------

const para = document.createElement('p');
document.body.appendChild(para);
para.appendChild(t);

say('tree.parentIsPara', t.parentNode === para);
say('tree.firstChildIsText', para.firstChild === t);
say('tree.lastChildIsText', para.lastChild === t);
say('tree.childNodesLen', para.childNodes.length);
// The element view of the same element: a text child is not an element child.
say('tree.childrenLen', para.children.length);
say('tree.firstElementChildNull', para.firstElementChild === null);
// And the text reaches the element's flattened content.
say('tree.paraTextContent', para.textContent);

// IDENTITY, by two routes and twice over. A childNodes read that rebuilt its
// wrapper would satisfy every getter above and still break every caller that
// compares.
say('tree.identityChildNodes', para.childNodes[0] === t);
say('tree.identityStable', para.childNodes[0] === para.childNodes[0]);
say('tree.contains', para.contains(t));
say('tree.containsSelf', t.contains(t));

// ---------------------------------------------------------------------------
// Siblings: the node walk and the element walk disagree, correctly
// ---------------------------------------------------------------------------

const em = document.createElement('em');
para.appendChild(em);
const tail = document.createTextNode('!');
para.appendChild(tail);

say('sib.nextIsEm', t.nextSibling === em);
say('sib.emNextIsTail', em.nextSibling === tail);
say('sib.emNextElementNull', em.nextElementSibling === null);
say('sib.tailPrevIsEm', tail.previousSibling === em);
say('sib.tailNextNull', tail.nextSibling === null);
say('sib.childNodesLen', para.childNodes.length);
say('sib.childrenLen', para.children.length);

// childNodes is a REAL array, like children is: what a caller does with it is
// iterate, and an object with numeric keys and a length is a TypeError at
// every one of those call sites.
let kinds = '';
for (const n of para.childNodes) kinds = kinds + n.nodeType + ',';
say('sib.kinds', kinds);
say('sib.isArray', Array.isArray(para.childNodes));

// ---------------------------------------------------------------------------
// CharacterData, with offsets that are UTF-16
// ---------------------------------------------------------------------------

const cd = document.createTextNode('abcdef');
cd.appendData('gh');
say('cd.append', cd.data);
cd.insertData(2, '--');
say('cd.insert', cd.data);
cd.deleteData(2, 2);
say('cd.delete', cd.data);
cd.replaceData(0, 3, 'XYZ');
say('cd.replace', cd.data);
say('cd.substring', cd.substringData(1, 3));
// Out of range clamps rather than throwing: there is no DOMException here.
say('cd.substringPastEnd', cd.substringData(4, 999));
say('cd.substringNegative', cd.substringData(-5, 2));

// One two-byte character and one four-byte one. In UTF-16 'é' is one unit and
// '🎉' is two, so this string is 5 units long and 9 bytes long — the two
// numbers an implementation can confuse.
const u16 = document.createTextNode('aé🎉b');
say('u16.length', u16.length);
say('u16.firstTwo', u16.substringData(0, 2));
// Offset 2 is the start of the surrogate pair; taking two units takes the
// whole character rather than half of it.
say('u16.pair', u16.substringData(2, 2));
u16.deleteData(2, 2);
say('u16.afterDelete', u16.data);

// splitText cuts in two and puts the tail next to the head, in the same parent.
const whole = document.createTextNode('headtail');
para.appendChild(whole);
const splitTail = whole.splitText(4);
say('split.head', whole.data);
say('split.tail', splitTail.data);
say('split.tailParent', splitTail.parentNode === para);
say('split.adjacency', whole.nextSibling === splitTail);
splitTail.remove();
whole.remove();
say('split.removed', para.childNodes.length);

// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

const c = document.createComment('a note');
para.appendChild(c);
say('comment.nodeType', c.nodeType);
say('comment.nodeName', c.nodeName);
say('comment.data', c.data);
say('comment.isChildNode', para.childNodes.length);
say('comment.notChildElement', para.children.length);
// A comment contributes nothing to the element's flattened text.
say('comment.notInTextContent', para.textContent);
c.remove();

// ---------------------------------------------------------------------------
// Fragments: build off-tree, insert once, and the fragment vanishes
// ---------------------------------------------------------------------------

const frag = document.createDocumentFragment();
say('frag.nodeType', frag.nodeType);
for (let i = 0; i < 3; i++) {
    const row = document.createElement('div');
    row.className = 'row';
    row.appendChild(document.createTextNode('row' + i));
    frag.appendChild(row);
}
say('frag.childNodesLen', frag.childNodes.length);
say('frag.childrenLen', frag.children.length);

const host = document.createElement('section');
document.body.appendChild(host);
host.appendChild(frag);
// The gain and the loss, together: inserting the fragment itself would satisfy
// only the first of these.
say('frag.hostGained', host.children.length);
say('frag.fragEmptied', frag.childNodes.length);
say('frag.rowsAreElements', host.firstElementChild.className);
say('frag.rowText', host.children[2].textContent);

// ---------------------------------------------------------------------------
// append() with a string, which the web turns into a text node
// ---------------------------------------------------------------------------

const app = document.createElement('div');
document.body.appendChild(app);
app.append('bare');
say('append.strLen', app.childNodes.length);
say('append.strType', app.firstChild.nodeType);
say('append.strText', app.textContent);
app.append(document.createElement('span'), ' and more');
say('append.mixedLen', app.childNodes.length);
say('append.mixedElems', app.children.length);

// ---------------------------------------------------------------------------
// cloneNode
// ---------------------------------------------------------------------------

const src = document.createElement('div');
src.id = 'source';
src.className = 'orig';
src.setAttribute('data-k', 'v');
src.appendChild(document.createTextNode('deep'));

const shallow = src.cloneNode(false);
say('clone.shallowIsNew', shallow !== src);
say('clone.shallowTag', shallow.tagName);
say('clone.shallowClass', shallow.className);
say('clone.shallowAttr', shallow.getAttribute('data-k'));
say('clone.shallowEmpty', shallow.childNodes.length);
// The one deliberate deviation from the spec, shared with bro's own JS
// binding: the id is NOT copied. Two nodes carrying one id is a bug an app
// almost never intends, and getElementById would answer whichever it reached
// first.
say('clone.shallowNoId', shallow.id === '');

const deep = src.cloneNode(true);
say('clone.deepChildren', deep.childNodes.length);
say('clone.deepText', deep.textContent);
say('clone.deepChildIsNew', deep.firstChild !== src.firstChild);

const clonedText = t.cloneNode(false);
say('clone.textData', clonedText.data);
say('clone.textIsNew', clonedText !== t);

// ---------------------------------------------------------------------------
// insertBefore and replaceChild, at node level
// ---------------------------------------------------------------------------

const box = document.createElement('div');
document.body.appendChild(box);
const one = document.createTextNode('one');
const two = document.createElement('i');
box.appendChild(two);
box.insertBefore(one, two);
say('ins.order', box.firstChild === one && box.lastChild === two);
const three = document.createTextNode('three');
box.replaceChild(three, one);
say('ins.replaced', box.firstChild === three);
say('ins.detached', one.parentNode === null);
say('ins.len', box.childNodes.length);
// insertBefore with a null ref appends — the spelling every UI library uses.
const four = document.createTextNode('four');
box.insertBefore(four, null);
say('ins.appendViaNull', box.lastChild === four);

// ---------------------------------------------------------------------------
// The authored page: markup this app did not write
// ---------------------------------------------------------------------------
// This is the case the elements-only DOM could not see at all. Four child
// nodes, one child element, and the first child is text — see index.html for
// why the count is a property of the markup rather than of its indentation.

const mixed = document.getElementById('mixed');
say('mixed.childNodesLen', mixed.childNodes.length);
say('mixed.childrenLen', mixed.children.length);
say('mixed.firstChildType', mixed.firstChild.nodeType);
// Bracketed, because the space at the end of "Hello " is the point: it is what
// separates the two words once <b> is flattened away, and a bare trailing space
// in the expectation file is the kind of thing an editor silently eats.
say('mixed.firstChildData', '[' + mixed.firstChild.data + ']');
say('mixed.firstElementTag', mixed.firstElementChild.tagName);
say('mixed.lastChildType', mixed.lastChild.nodeType);
say('mixed.lastChildData', '[' + mixed.lastChild.data + ']');
say('mixed.textContent', '[' + mixed.textContent + ']');
say('mixed.boldNextData', '[' + mixed.firstElementChild.nextSibling.data + ']');
say('mixed.boldPrevData', '[' + mixed.firstElementChild.previousSibling.data + ']');
say('mixed.textParent', mixed.firstChild.parentNode === mixed);
say('mixed.contains', mixed.contains(mixed.firstChild));

// Editing one word of it, without rewriting the rest — the thing textContent
// alone cannot do.
mixed.firstChild.data = 'Goodbye ';
say('mixed.edited', '[' + mixed.textContent + ']');

// ---------------------------------------------------------------------------
// Done, one frame later
// ---------------------------------------------------------------------------

requestAnimationFrame(function () {
    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
});
