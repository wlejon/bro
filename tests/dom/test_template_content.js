// <template> contents survive parsing, and live in an inert content fragment.
//
// gumbo emits templates as GUMBO_NODE_TEMPLATE, a node type deliberately kept
// separate from GUMBO_NODE_ELEMENT so clients can choose whether to descend.
// bro's tree builder only ever matched GUMBO_NODE_ELEMENT, so both the
// <template> element AND everything inside it silently vanished from any
// document parsed straight through gumbo — DOMParser and innerHTML. Only the
// app-HTML path escaped it, because extractTemplates() rewrites templates to
// placeholders before gumbo ever sees them.
//
// Per spec the children go into a separate `content` DocumentFragment rather
// than the normal child list, which is what makes them inert: not laid out,
// not painted, not reachable from a document query.

const root = document.getElementById('root');
const parser = new DOMParser();

// ---------------------------------------------------------------------------
// DOMParser
// ---------------------------------------------------------------------------
const doc = parser.parseFromString(
    '<html><body><template id="t"><div class="x">hi</div>' +
    '<span class="y">yo</span></template><p id="after">after</p></body></html>',
    'text/html');

const t = doc.querySelector('#t');
assert(t !== null, 'template element survived parsing');
assert(t.tagName.toUpperCase() === 'TEMPLATE', 'it is a <template>');
assert(doc.querySelector('#after') !== null, 'sibling after the template survived too');

// Contents are NOT normal children.
assert(t.childNodes.length === 0, 'template has no normal children');

// ...they are in .content.
const content = t.content;
assert(content, 'template.content exists');
assert(content.querySelector('.x') !== null, 'content.querySelector finds the div');
assert(content.querySelector('.x').textContent === 'hi', 'content is fully parsed');
assert(content.querySelector('.y') !== null, 'second child parsed too');
assert(content.childNodes.length === 2, 'both children are in the fragment');

// .content is stable — the same fragment every read, so mutations stick.
assert(t.content === content, 'content is the same fragment on every read');
content.querySelector('.x').textContent = 'mutated';
assert(t.content.querySelector('.x').textContent === 'mutated', 'mutations to content stick');

// Inert: invisible to document queries.
assert(doc.querySelector('.x') === null, 'template content is not reachable from the document');
assert(doc.getElementsByTagName('div').length === 0, 'not found by tag either');

// ---------------------------------------------------------------------------
// Not rendered while it lives in the template
// ---------------------------------------------------------------------------
root.innerHTML = '<template id="it"><b class="z">bold</b></template><p>visible</p>';
flush();

const it = document.getElementById('it');
assert(it !== null, 'template survived innerHTML parsing');
assert(it.content.querySelector('.z') !== null, 'innerHTML template content parsed');
assert(document.querySelector('.z') === null, 'template content not in the live document');
assert(root.textContent.indexOf('bold') === -1,
       'template content does not render, got: ' + JSON.stringify(root.textContent));
assert(root.textContent.indexOf('visible') !== -1, 'the sibling paragraph does render');

// ---------------------------------------------------------------------------
// Cloning content into the live document renders it
// ---------------------------------------------------------------------------
const clone = it.content.cloneNode(true);
assert(clone.childNodes.length === 1, 'deep clone carried the children');
root.appendChild(clone);
flush();

const live = document.querySelector('.z');
assert(live !== null, 'cloned content is in the live document');
assert(live.textContent === 'bold', 'cloned content kept its text');
assert(live.getBoundingClientRect().width > 0, 'cloned content actually lays out');
// The clone is a copy: the template still holds its own children.
assert(it.content.querySelector('.z') !== null, 'template content survives cloning');
assert(it.content.querySelector('.z') !== live, 'clone is a distinct node');

// ---------------------------------------------------------------------------
// Cross-document: clone a parsed template's content straight into the live
// tree. Pre-insertion adopts it (see test_adopt_node.js).
// ---------------------------------------------------------------------------
const holder = document.createElement('div');
root.appendChild(holder);
holder.appendChild(t.content.cloneNode(true));
flush();

const adopted = holder.querySelector('.x');
assert(adopted !== null, 'parsed-document template content cloned into the live tree');
assert(adopted.ownerDocument === document, 'it was adopted into the live document');
assert(adopted.textContent === 'mutated', 'the mutated content came across');
assert(adopted.getBoundingClientRect().width > 0, 'and it renders');

// ---------------------------------------------------------------------------
// An empty template still has a usable, stable content fragment.
// ---------------------------------------------------------------------------
const emptyDoc = parser.parseFromString(
    '<html><body><template id="e"></template></body></html>', 'text/html');
const e = emptyDoc.querySelector('#e');
assert(e !== null, 'empty template survived parsing');
const ec = e.content;
assert(ec, 'empty template has a content fragment');
assert(ec.childNodes.length === 0, 'and it is empty');
for (let i = 0; i < 6; i++) flush();   // orphan sweep must not eat it
assert(e.content === ec, 'empty content fragment survives GC sweeps');

// ---------------------------------------------------------------------------
// Nested templates
// ---------------------------------------------------------------------------
const nestDoc = parser.parseFromString(
    '<html><body><template id="outer"><div class="mid">' +
    '<template id="inner"><i class="deep">deep</i></template></div>' +
    '</template></body></html>', 'text/html');
const outer = nestDoc.querySelector('#outer');
assert(outer !== null, 'outer template survived');
const mid = outer.content.querySelector('.mid');
assert(mid !== null, 'outer content parsed');
const inner = outer.content.querySelector('#inner');
assert(inner !== null, 'nested template is inside the outer content');
assert(inner.content.querySelector('.deep') !== null, 'nested template content parsed');
assert(outer.content.querySelector('.deep') === null,
       'nested content stays in its own fragment, not the outer one');

console.log('template content: OK');
