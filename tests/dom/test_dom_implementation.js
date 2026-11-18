// document.implementation and the document as a node, with Chromium's answers:
// createHTMLDocument / createDocument / createDocumentType, doctype nodes,
// documentElement.parentNode === document, cloneNode / importNode / adoptNode
// of and into documents, and documents a script made having no window.
const T = (v) => Object.prototype.toString.call(v).slice(8, -1);
const eq = (got, want, what) => assert(got === want, `${what}: got ${got}, want ${want}`);
const impl = document.implementation;
eq(`${T(impl)} ${impl === document.implementation} ${impl instanceof DOMImplementation}`, 'DOMImplementation true true', 'implementation');
eq(impl.hasFeature(), true, 'hasFeature');

const h = impl.createHTMLDocument('t1');
eq(`${T(h)} ${h instanceof Document} ${h.title} ${h.documentElement.outerHTML}`,
   'HTMLDocument true t1 <html><head><title>t1</title></head><body></body></html>', 'createHTMLDocument');
eq(impl.createHTMLDocument().documentElement.outerHTML, '<html><head></head><body></body></html>', 'no title');
eq(impl.createHTMLDocument('').documentElement.outerHTML, '<html><head><title></title></head><body></body></html>', 'empty title');
eq(impl.createHTMLDocument('<b>').title, '<b>', 'a title is text');
eq(`${T(h.doctype)} ${h.doctype.name} ${h.doctype.nodeType} ${h.firstChild === h.doctype} ${h.childNodes.length}`,
   'DocumentType html 10 true 2', 'doctype');
eq(`${h.documentElement.parentNode === h} ${h.documentElement.previousSibling === h.doctype} ${h.doctype.nextSibling === h.documentElement}`,
   'true true true', 'the document is the parent of its children');
eq(`${document.documentElement.parentNode === document} ${document.nodeName} ${document.parentNode}`, 'true #document null', 'the page');
const d = h.createElement('div');
d.innerHTML = '<p class=a>x</p><p>y</p>';
h.body.appendChild(d);
eq(`${d.ownerDocument === h} ${h.querySelectorAll('p').length} ${h.querySelector('.a').textContent}`, 'true 2 x', 'a created document works');
const base = h.createElement('base');
base.href = 'http://example.com/dir/';
h.head.appendChild(base);
const link = h.createElement('a');
link.href = 'x.html';
eq(link.href, 'http://example.com/dir/x.html', 'its <base> resolves its links');
eq(`${h.URL} ${h.defaultView} ${h.location}`, 'about:blank null null', 'no window');

const x = impl.createDocument('', '', null);
eq(`${T(x)} ${x instanceof XMLDocument} ${x.documentElement} ${x.childNodes.length} ${x.contentType}`,
   'XMLDocument true null 0 application/xml', 'createDocument without a root');
const pm = x.createElementNS('http://www.opengis.net/kml/2.2', 'kml:Placemark');
const name = x.createElement('name');
name.textContent = 'a<b';
pm.appendChild(name);
x.appendChild(pm);
eq(`${name.tagName} ${x.documentElement === pm} ${new XMLSerializer().serializeToString(x)}`,
   'name true <kml:Placemark xmlns:kml="http://www.opengis.net/kml/2.2"><name>a&lt;b</name></kml:Placemark>', 'an XML document');
let err = null;
try { x.appendChild(x.createElement('second')); } catch (e) { err = e.name; }
eq(err, 'HierarchyRequestError', 'one element per document');
const svgDoc = impl.createDocument('http://www.w3.org/2000/svg', 'svg', null);
eq(`${svgDoc.contentType} ${T(svgDoc.documentElement)}`, 'image/svg+xml SVGSVGElement', 'an SVG document');
const dt = impl.createDocumentType('html', '-//W3C//DTD XHTML 1.0 Strict//EN', 'http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd');
const xh = impl.createDocument('http://www.w3.org/1999/xhtml', 'html', dt);
eq(`${xh.doctype === dt} ${dt.ownerDocument === xh} ${xh.contentType} ${new XMLSerializer().serializeToString(xh)}`,
   'true true application/xhtml+xml <!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Strict//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"><html xmlns="http://www.w3.org/1999/xhtml"></html>',
   'a doctype given to createDocument');

document.body.innerHTML = '<div id="x" class="k">hi<b>b</b></div>';
const c = document.cloneNode(true);
eq(`${T(c)} ${c !== document} ${c.querySelector('#x').innerHTML} ${c.getElementById('x') !== document.getElementById('x')}`,
   'HTMLDocument true hi<b>b</b> true', 'document.cloneNode(true)');
const shallow = document.cloneNode(false);
eq(`${shallow.childNodes.length} ${shallow.documentElement}`, '0 null', 'document.cloneNode(false)');
const imported = h.importNode(document.getElementById('x'), true);
eq(`${imported.ownerDocument === h} ${imported.outerHTML}`, 'true <div id="x" class="k">hi<b>b</b></div>', 'importNode');
const span = document.createElement('span');
document.body.appendChild(span);
eq(`${h.adoptNode(span) === span} ${span.ownerDocument === h} ${span.parentNode}`, 'true true null', 'adoptNode');
err = null;
try { h.adoptNode(document); } catch (e) { err = e.name; }
eq(err, 'NotSupportedError', 'a document cannot be adopted');
const nd = new Document();
eq(`${T(nd)} ${nd.childNodes.length} ${nd.contentType} ${nd.createElement('DIV').namespaceURI}`, 'Document 0 application/xml null', 'new Document()');
console.log('test_dom_implementation: OK');
