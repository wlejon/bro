// DOMParser's XML types run an XML parser: names keep their case, `<x/>` is
// an empty element, namespaces resolve, entities and CDATA decode, and a
// document that is not well-formed comes back as a <parsererror> document
// instead of whatever the HTML parser made of it.

const p = new DOMParser();

// --- application/xml -------------------------------------------------------------
const xml = p.parseFromString(
    '<?xml version="1.0"?>\n<!-- lead --><Root a="1 &amp; 2"><Item id="i1"/><Item>x &lt; y</Item>' +
    '<data><![CDATA[<raw & text>]]></data><ex:Thing xmlns:ex="urn:ex" ex:k="v">&#65;&#x42;</ex:Thing></Root>',
    'application/xml');
const root = xml.documentElement;
assert(xml.contentType === 'application/xml', 'contentType, got ' + xml.contentType);
assert(root.nodeName === 'Root', 'documentElement keeps its case, got ' + root.nodeName);
assert(root.tagName === 'Root', 'tagName keeps its case, got ' + root.tagName);
assert(root.namespaceURI === null, 'no default namespace is the null namespace, got ' + root.namespaceURI);
assert(root.getAttribute('a') === '1 & 2', 'attribute entity decoded, got ' + root.getAttribute('a'));
assert(xml.body == null, 'an XML document has no body');
const kids = root.children;
assert(kids.length === 4, 'four children, got ' + kids.length);
assert(kids[0].nodeName === 'Item' && kids[0].childNodes.length === 0, '<Item/> is empty, not an open tag');
assert(kids[1].textContent === 'x < y', 'text entity decoded, got ' + kids[1].textContent);
assert(kids[2].textContent === '<raw & text>', 'CDATA is text, got ' + kids[2].textContent);
assert(kids[3].tagName === 'ex:Thing' && kids[3].localName === 'Thing' && kids[3].prefix === 'ex',
    'prefixed name, got ' + kids[3].tagName);
assert(kids[3].namespaceURI === 'urn:ex', 'prefix resolves to its xmlns, got ' + kids[3].namespaceURI);
assert(kids[3].textContent === 'AB', 'character references, got ' + kids[3].textContent);
assert(xml.querySelector('parsererror') === null, 'a well-formed document has no parsererror');
assert(xml.getElementById('i1') === kids[0], 'getElementById');

// --- image/svg+xml -----------------------------------------------------------------
const svg = p.parseFromString(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><defs><linearGradient id="g"/></defs>' +
    '<rect width="10" height="10"/></svg>', 'image/svg+xml');
const s = svg.documentElement;
assert(s.nodeName === 'svg', 'svg root, got ' + s.nodeName);
assert(s.namespaceURI === 'http://www.w3.org/2000/svg', 'default namespace applies, got ' + s.namespaceURI);
const lg = s.querySelector('linearGradient') || s.firstElementChild.firstElementChild;
assert(lg && lg.tagName === 'linearGradient', 'camelCase kept, got ' + (lg && lg.tagName));
assert(lg.namespaceURI === 'http://www.w3.org/2000/svg', 'namespace inherited');
assert(s.lastElementChild.nodeName === 'rect', 'self-closed <linearGradient/> did not swallow <rect>');

// --- not well-formed ---------------------------------------------------------------------
for (const bad of ['<a><b></a>', '<a>', '<a x=1/>', '<a>&nope;</a>', '', '<a/><b/>', '<p:a/>']) {
    const d = p.parseFromString(bad, 'application/xml');
    const perr = d.querySelector('parsererror');
    assert(perr !== null, 'parsererror for ' + JSON.stringify(bad));
    assert(d.documentElement === perr, 'the parsererror is the root for ' + JSON.stringify(bad));
    assert(/XML Parsing Error/.test(perr.textContent) && /Line Number 1/.test(perr.textContent),
        'the error names what and where: ' + perr.textContent);
}

// text/html still runs the HTML parser.
const html = p.parseFromString('<p>x', 'text/html');
assert(html.body && html.body.firstElementChild.tagName === 'P', 'text/html is unchanged');
