// A <template> built by script: `content` is a DocumentFragment distinct from
// the template, innerHTML writes go INTO that fragment (never the template's
// own child list), and innerHTML / outerHTML read back from it.

const t = document.createElement('template');
assert(t instanceof HTMLTemplateElement, 'createElement gives an HTMLTemplateElement');

const c0 = t.content;
assert(c0 && c0 !== t, 'content is not the template itself');
assert(c0.nodeType === 11, 'content is a DocumentFragment node, got nodeType ' + c0.nodeType);
assert(c0 instanceof DocumentFragment, 'content instanceof DocumentFragment');
assert(c0.childNodes.length === 0, 'a fresh template has empty content');
assert(t.content === c0, 'content is stable');

t.innerHTML = '<b class="tb">x</b><i class="ti">y</i>';
assert(t.childNodes.length === 0, 'innerHTML did not create template children');
assert(t.content === c0, 'innerHTML fills the SAME fragment');
assert(c0.childNodes.length === 2, 'content holds the parsed children, got ' + c0.childNodes.length);
assert(c0.querySelector('.tb').textContent === 'x', 'content is queryable');
assert(t.innerHTML === '<b class="tb">x</b><i class="ti">y</i>',
       'innerHTML reads the content back, got ' + JSON.stringify(t.innerHTML));
assert(t.outerHTML.indexOf('<b class="tb">x</b>') !== -1,
       'outerHTML includes the content, got ' + JSON.stringify(t.outerHTML));

// Replacing the markup replaces the content's children.
t.innerHTML = '<p>only</p>';
assert(c0.childNodes.length === 1 && c0.firstChild.tagName === 'P', 'innerHTML replaces');

// Inert while attached.
const root = document.getElementById('root');
root.appendChild(t);
flush();
assert(document.querySelector('p') === null, 'content is not reachable from the document');
assert(t.getBoundingClientRect().height === 0, 'a template does not render');

// Stamping it out.
const stamped = t.content.cloneNode(true);
assert(stamped.nodeType === 11, 'cloned content is a fragment');
root.appendChild(stamped);
flush();
const live = root.querySelector('p');
assert(live && live.textContent === 'only', 'stamped content is live');
assert(live.getBoundingClientRect().height > 0, 'and renders');
assert(t.content.childNodes.length === 1, 'the template keeps its content');

// Table-row markup survives inside a template.
const rows = document.createElement('template');
rows.innerHTML = '<tr><td>a</td><td>b</td></tr>';
const tr = rows.content.querySelector('tr');
assert(tr !== null, 'a <tr> parses into template content');
assert(tr.querySelectorAll('td').length === 2, 'with its cells');

// Parsed via an ancestor's innerHTML: the same shape.
const holder = document.createElement('div');
holder.innerHTML = '<template id="nested-t"><span class="ns">n</span></template>';
const nt = holder.querySelector('#nested-t');
assert(nt.childNodes.length === 0, 'parsed template has no children');
assert(nt.content.nodeType === 11 && nt.content !== nt, 'parsed template has a fragment');
assert(nt.content.querySelector('.ns').textContent === 'n', 'parsed content');
assert(nt.innerHTML === '<span class="ns">n</span>', 'parsed template innerHTML reads content');
assert(!nt.hasAttribute('data-bro-template-html'), 'no placeholder attribute leaks');

// A full document from DOMParser, template in <head> and <body>.
const doc = new DOMParser().parseFromString(
    '<html><head><template id="h"><i>h</i></template></head>' +
    '<body><template id="b"><u>b</u></template></body></html>', 'text/html');
assert(doc.getElementById('h').content.querySelector('i') !== null, 'head template content');
assert(doc.getElementById('b').content.querySelector('u') !== null, 'body template content');

console.log('template innerHTML/content: OK');
