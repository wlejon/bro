// The namespaced attribute methods (host_element_attrs_ns.cpp):
// setAttributeNS / getAttributeNS / hasAttributeNS / removeAttributeNS on an
// SVG element made by createElementNS (TweenMax's CSSPlugin feature test),
// an xlink:href set and read back by namespace, and the qualified-name
// lookups of getElementsByTagName on an XML document.

const SVG = 'http://www.w3.org/2000/svg';
const XLINK = 'http://www.w3.org/1999/xlink';

const rect = document.createElementNS(SVG, 'rect');
for (const m of ['setAttributeNS', 'getAttributeNS', 'hasAttributeNS', 'removeAttributeNS'])
    assert(typeof rect[m] === 'function', m + ' is a method of an SVG element');
rect.setAttributeNS(null, 'x', '1');
assert(rect.getAttribute('x') === '1' && rect.getAttributeNS(null, 'x') === '1', 'a null-namespace attribute');
assert(rect.hasAttributeNS(null, 'x') && !rect.hasAttributeNS(XLINK, 'x'), 'hasAttributeNS tells namespaces apart');

const use = document.createElementNS(SVG, 'use');
use.setAttributeNS(XLINK, 'xlink:href', '#a');
assert(use.getAttribute('xlink:href') === '#a', 'stored under its qualified name');
assert(use.getAttributeNS(XLINK, 'href') === '#a', 'read by namespace and local name');
assert(use.getAttributeNS(null, 'href') === null, 'not in the null namespace');
use.setAttributeNS(XLINK, 'xl:href', '#b');
assert(use.getAttributeNS(XLINK, 'href') === '#b' && use.getAttribute('xlink:href') === '#b',
       'setAttributeNS replaces the attribute of that namespace and local name');
use.removeAttributeNS(XLINK, 'href');
assert(!use.hasAttributeNS(XLINK, 'href') && use.getAttribute('xlink:href') === null, 'removeAttributeNS');

let threw = '';
try { use.setAttributeNS(null, 'p:a', 'v'); } catch (e) { threw = e.name; }
assert(threw === 'NamespaceError', 'a prefix without a namespace is a NamespaceError, got ' + threw);

const xml = new DOMParser().parseFromString('<a:R xmlns:a="u"><a:D k="1"/><Foo/></a:R>', 'text/xml');
assert(xml.getElementsByTagName('a:D').length === 1, 'getElementsByTagName by qualified name in an XML document');
assert(xml.getElementsByTagName('Foo').length === 1 && xml.getElementsByTagName('foo').length === 0,
       'an XML document\'s names match case-sensitively');
document.body.innerHTML = '<p>x</p>';
assert(document.getElementsByTagName('P').length === 1, 'an HTML element matches its name in any case');

console.log('test_attributes_ns: OK');
