// Element.namespaceURI / localName: XHTML for HTML elements, SVG and MathML
// for foreign content (created with createElementNS or parsed inside <svg> /
// <math>), null for the null namespace.

const XHTML = 'http://www.w3.org/1999/xhtml';
const SVG = 'http://www.w3.org/2000/svg';
const MATHML = 'http://www.w3.org/1998/Math/MathML';

// createElement: always HTML, even for a tag that names an SVG element.
assert(document.createElement('div').namespaceURI === XHTML, 'div is XHTML');
assert(document.createElement('svg').namespaceURI === XHTML,
       'createElement("svg") is an HTML element (as on the web)');
assert(document.body.namespaceURI === XHTML, 'body is XHTML');
assert(document.documentElement.namespaceURI === XHTML, 'html is XHTML');
assert(document.createElement('DIV').localName === 'div', 'localName is lower case');

// createElementNS.
const svg = document.createElementNS(SVG, 'svg');
assert(svg.namespaceURI === SVG, 'createElementNS svg');
assert(svg.localName === 'svg', 'svg localName');
const circle = document.createElementNS(SVG, 'circle');
assert(circle.namespaceURI === SVG, 'createElementNS circle');
const math = document.createElementNS(MATHML, 'math');
assert(math.namespaceURI === MATHML, 'createElementNS math');
const xdiv = document.createElementNS(XHTML, 'div');
assert(xdiv.namespaceURI === XHTML, 'createElementNS XHTML div');
const nul = document.createElementNS(null, 'thing');
assert(nul.namespaceURI === null, 'null namespace reads null, got ' + nul.namespaceURI);
const custom = document.createElementNS('urn:example', 'ex:item');
assert(custom.namespaceURI === 'urn:example', 'an arbitrary namespace is kept');
assert(custom.localName === 'item', 'the prefix is not part of localName');

// Parser: innerHTML into an HTML element.
const host = document.createElement('div');
host.innerHTML = '<p>t</p><svg viewBox="0 0 10 10"><circle r="2"/><g><rect/></g></svg>' +
                 '<math><mi>x</mi></math>';
document.body.appendChild(host);
assert(host.querySelector('p').namespaceURI === XHTML, 'parsed p is XHTML');
assert(host.querySelector('svg').namespaceURI === SVG, 'parsed svg is SVG');
assert(host.querySelector('circle').namespaceURI === SVG, 'parsed circle is SVG');
assert(host.querySelector('rect').namespaceURI === SVG, 'nested rect is SVG');
assert(host.querySelector('math').namespaceURI === MATHML, 'parsed math is MathML');
assert(host.querySelector('mi').namespaceURI === MATHML, 'parsed mi is MathML');

// Parser: innerHTML INTO an SVG element parses as SVG content.
const svgHost = host.querySelector('svg');
svgHost.innerHTML = '<ellipse rx="1" ry="1"/><text>hi</text>';
assert(svgHost.querySelector('ellipse') !== null, 'svg innerHTML parsed');
assert(svgHost.querySelector('ellipse').namespaceURI === SVG,
       'svg.innerHTML children are SVG, got ' + svgHost.querySelector('ellipse').namespaceURI);
assert(svgHost.querySelector('text').namespaceURI === SVG, 'svg text is SVG');
svg.innerHTML = '<circle r="1"/>';
assert(svg.firstChild && svg.firstChild.namespaceURI === SVG,
       'innerHTML into a createElementNS svg is SVG');

// foreignObject content is HTML again.
host.innerHTML = '<svg><foreignObject><div class="fo">x</div></foreignObject></svg>';
assert(host.querySelector('.fo').namespaceURI === XHTML, 'foreignObject content is XHTML');

// DOMParser documents.
const doc = new DOMParser().parseFromString(
    '<html><body><svg><path d="M0 0"/></svg><span></span></body></html>', 'text/html');
assert(doc.querySelector('path').namespaceURI === SVG, 'DOMParser svg path is SVG');
assert(doc.querySelector('span').namespaceURI === XHTML, 'DOMParser span is XHTML');

// Clones keep the namespace.
assert(circle.cloneNode(false).namespaceURI === SVG, 'shallow clone keeps SVG');
const deep = host.querySelector('svg').cloneNode(true);
assert(deep.querySelector('div').namespaceURI === XHTML, 'deep clone keeps XHTML inside');
assert(deep.namespaceURI === SVG, 'deep clone keeps SVG');
assert(custom.cloneNode(false).namespaceURI === 'urn:example', 'clone keeps an arbitrary ns');
assert(nul.cloneNode(false).namespaceURI === null, 'clone keeps the null ns');

// Fragments have no namespaceURI.
assert(document.createDocumentFragment().namespaceURI === undefined,
       'a DocumentFragment has no namespaceURI');

console.log('namespaceURI: OK');
