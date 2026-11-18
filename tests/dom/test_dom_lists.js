// The DOM's list interfaces (host_dom_lists.h): NodeList and HTMLCollection
// exist as interfaces (`x instanceof NodeList`, as A-Frame's bundled anime.js
// tests), querySelectorAll is a static NodeList, childNodes a live NodeList,
// children a live HTMLCollection (each the same object at every read), and
// the document's and forms' standard collections are there and live:
// document.forms / images / links / anchors / scripts / embeds / children,
// form.elements, select.options (whose length truncates) and selectedOptions.

document.body.innerHTML = '<form id="f"><input name="a"><select name="s"><option>x</option>' +
    '<option selected>y</option></select></form><img><a href="#">l</a><a name="n">m</a><a>plain</a>';
const body = document.body;

for (const name of ['NodeList', 'HTMLCollection', 'HTMLOptionsCollection', 'HTMLFormControlsCollection']) {
    assert(typeof globalThis[name] === 'function', name + ' is an interface object');
    let threw = '';
    try { new globalThis[name](); } catch (e) { threw = e.name; }
    assert(threw === 'TypeError', 'new ' + name + '() is an Illegal constructor, got ' + threw);
}
assert(Object.getPrototypeOf(HTMLOptionsCollection.prototype) === HTMLCollection.prototype,
       'HTMLOptionsCollection extends HTMLCollection');

const qsa = document.querySelectorAll('a');
assert(qsa instanceof NodeList && !Array.isArray(qsa), 'querySelectorAll is a NodeList');
assert(qsa.length === 3 && qsa.item(1) === qsa[1] && qsa.item(9) === null, 'NodeList length / item');
assert(Object.keys(qsa).join() === '0,1,2', 'a NodeList enumerates its indices, got ' + Object.keys(qsa).join());
const seen = [];
qsa.forEach((a, i) => seen.push(i + a.tagName));
assert(seen.join() === '0A,1A,2A', 'NodeList.forEach, got ' + seen.join());
body.appendChild(document.createElement('a'));
assert(qsa.length === 3, 'querySelectorAll is static');

const kids = body.childNodes;
assert(kids instanceof NodeList && kids === body.childNodes, 'childNodes is one NodeList');
const n0 = kids.length;
body.appendChild(document.createElement('p'));
assert(kids.length === n0 + 1, 'childNodes is live');

const children = body.children;
assert(children instanceof HTMLCollection && children === body.children, 'children is one HTMLCollection');
assert(children.forEach === undefined && [...children].length === children.length, 'an HTMLCollection iterates, no forEach');
assert(children.namedItem('f') === children.f && children.f.tagName === 'FORM', 'named access by id');
assert(children.item === HTMLCollection.prototype.item, 'members come from the prototype');

const docLists = { forms: 1, images: 1, links: 1, anchors: 1, scripts: 0, embeds: 0, children: 1 };
for (const [name, count] of Object.entries(docLists)) {
    const list = document[name];
    assert(list instanceof HTMLCollection, 'document.' + name + ' is an HTMLCollection');
    assert(list.length === count, 'document.' + name + '.length is ' + count + ', got ' + list.length);
}
const form = document.forms[0];
assert(document.forms.f === form, 'document.forms by id');
document.body.appendChild(document.createElement('img'));
assert(document.images.length === 2, 'document.images is live');

assert(form.elements instanceof HTMLFormControlsCollection, 'form.elements is an HTMLFormControlsCollection');
assert(form.elements.length === 2 && form.elements.a === form.querySelector('input'), 'form.elements by name');

const select = form.querySelector('select');
assert(select.options instanceof HTMLOptionsCollection && select.options.length === 2, 'select.options');
const sel = select.selectedOptions;
assert(sel instanceof HTMLCollection && sel.length === 1 && sel[0].textContent === 'y', 'selectedOptions');
select.selectedIndex = 0;
assert(sel[0].textContent === 'x', 'selectedOptions is live');
select.options.length = 0;
assert(select.options.length === 0 && select.children.length === 0, 'options.length = 0 empties the select');

assert(document.getElementsByName('a') instanceof NodeList, 'getElementsByName is a NodeList');

console.log('test_dom_lists: OK');
