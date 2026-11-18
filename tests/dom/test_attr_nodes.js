// Attr nodes and NamedNodeMap as Chromium has them: element.attributes is a
// live NamedNodeMap of Attr objects (one per attribute, by index and by
// name), an Attr's value / nodeValue / textContent read and write the
// attribute, ownerElement follows it, and createAttribute /
// setAttributeNode / removeAttributeNode move Attrs on and off elements.

const el = document.createElement('div');
el.setAttribute('id', 'd');
el.setAttribute('title', 'hi');
el.setAttribute('class', 'c');
document.body.appendChild(el);

const attrs = el.attributes;
assert(attrs instanceof NamedNodeMap && attrs === el.attributes, 'one live NamedNodeMap');
assert(attrs.length === 3 && attrs[0].name === 'id' && attrs[0].value === 'd', 'index access');
assert(attrs.title.value === 'hi' && attrs.getNamedItem('class').value === 'c', 'name access');
assert(Object.keys(attrs).join(',') === '0,1,2', 'keys: ' + Object.keys(attrs).join(','));
assert([...attrs].map(a => a.name).join(',') === 'id,title,class', 'iteration');

const title = el.getAttributeNode('title');
assert(title === attrs[1] && title instanceof Attr && title instanceof Node, 'one Attr per attribute');
assert(title.nodeType === Node.ATTRIBUTE_NODE && title.nodeName === 'title', 'nodeType / nodeName');
assert(title.ownerElement === el && title.specified, 'ownerElement');
title.value = 'v2';
assert(el.getAttribute('title') === 'v2', 'value =');
title.nodeValue = 'v3';
assert(el.getAttribute('title') === 'v3', 'nodeValue =');
title.textContent = 'v4';
assert(el.getAttribute('title') === 'v4', 'textContent =');
el.setAttribute('title', 'v5');
assert(title.value === 'v5', 'an Attr follows setAttribute');

const foo = document.createAttribute('Foo');
assert(foo.name === 'foo' && foo.value === '' && foo.ownerElement === null, 'createAttribute lowercases, no owner');
foo.value = 'bar';
assert(el.setAttributeNode(foo) === null, 'setAttributeNode of a new name returns null');
assert(el.getAttribute('foo') === 'bar' && foo.ownerElement === el, 'and sets it');
el.removeAttributeNode(foo);
assert(!el.hasAttribute('foo') && foo.ownerElement === null && foo.value === 'bar', 'removeAttributeNode detaches');

el.removeAttribute('title');
assert(title.ownerElement === null && title.value === 'v5', 'removeAttribute detaches the Attr, keeping its value');

let threw = '';
const cls = el.getAttributeNode('class');
try { document.body.setAttributeNode(cls); } catch (e) { threw = e.name; }
assert(threw === 'InUseAttributeError', 'an Attr in use elsewhere: ' + threw);
assert(Object.prototype.toString.call(cls) === '[object Attr]', 'toStringTag Attr');
assert(Object.prototype.toString.call(attrs) === '[object NamedNodeMap]', 'toStringTag NamedNodeMap');

console.log('test_attr_nodes: OK');
