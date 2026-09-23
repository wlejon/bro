// createElementNS keeps the qualified name's case (DOM §4.5 "create an
// element": no case folding outside createElement), so SVG's camelCase
// elements read back as written — and the SVG paint path still recognises
// them, whichever way they were made: createElementNS, or markup parsed
// inside <svg> (the HTML parser's SVG tag-name adjustment).

const SVG = 'http://www.w3.org/2000/svg';
const XHTML = 'http://www.w3.org/1999/xhtml';

// ---- names -------------------------------------------------------------------
const lg = document.createElementNS(SVG, 'linearGradient');
assert(lg.tagName === 'linearGradient', 'tagName keeps case, got ' + lg.tagName);
assert(lg.localName === 'linearGradient', 'localName keeps case, got ' + lg.localName);
assert(lg.nodeName === 'linearGradient', 'nodeName keeps case, got ' + lg.nodeName);
assert(lg.prefix === null, 'no prefix, got ' + lg.prefix);
assert(lg.namespaceURI === SVG, 'SVG namespace');

for (const name of ['clipPath', 'radialGradient', 'feGaussianBlur', 'foreignObject', 'textPath']) {
    const el = document.createElementNS(SVG, name);
    assert(el.tagName === name && el.localName === name, name + ' keeps its case');
}
assert(document.createElementNS(SVG, 'svg').tagName === 'svg', 'an SVG svg is lower case');
assert(document.createElementNS(SVG, 'rect').tagName === 'rect', 'an SVG rect is lower case');

// HTML elements: createElement folds, and tagName is upper case in an HTML doc.
assert(document.createElement('DiV').tagName === 'DIV', 'createElement upper-cases tagName');
assert(document.createElement('DiV').localName === 'div', 'createElement lower-cases localName');
const xdiv = document.createElementNS(XHTML, 'div');
assert(xdiv.tagName === 'DIV' && xdiv.localName === 'div', 'XHTML createElementNS div');
// createElement on a camelCase name is still an HTML element, folded.
assert(document.createElement('linearGradient').localName === 'lineargradient',
       'createElement folds even an SVG-looking name');

// Prefixes and other namespaces.
const item = document.createElementNS('urn:example', 'ex:ItemName');
assert(item.tagName === 'ex:ItemName', 'qualified tagName, got ' + item.tagName);
assert(item.localName === 'ItemName', 'localName after the prefix, got ' + item.localName);
assert(item.prefix === 'ex', 'prefix, got ' + item.prefix);
const nul = document.createElementNS(null, 'Thing');
assert(nul.tagName === 'Thing' && nul.localName === 'Thing', 'null namespace keeps case');

let nsErr = null;
try { document.createElementNS(null, 'ex:thing'); } catch (e) { nsErr = e; }
assert(nsErr && nsErr.name === 'NamespaceError', 'a prefix with no namespace is a NamespaceError');

// Clones keep the name.
assert(lg.cloneNode(false).tagName === 'linearGradient', 'a clone keeps the case');
assert(item.cloneNode(false).tagName === 'ex:ItemName', 'a clone keeps the prefix');

// Serialization writes the name back as created.
const holder = document.createElementNS(SVG, 'svg');
holder.appendChild(document.createElementNS(SVG, 'linearGradient'));
assert(holder.innerHTML.indexOf('<linearGradient') === 0,
       'innerHTML serializes camelCase, got ' + holder.innerHTML);

// ---- parser: markup inside <svg> ------------------------------------------------
const box = document.createElement('div');
box.innerHTML = '<svg><defs><LINEARGRADIENT id="p"></LINEARGRADIENT>' +
                '<clippath id="c"></clippath></defs></svg>';
const parsed = box.querySelector('#p');
assert(parsed.tagName === 'linearGradient',
       'parsed SVG element is case-adjusted, got ' + parsed.tagName);
assert(box.querySelector('#c').localName === 'clipPath', 'clipPath adjusted');
assert(box.querySelector('svg').tagName === 'svg', 'parsed svg is lower case');
assert(box.querySelector('div') === null && box.tagName === 'DIV', 'HTML stays upper case');
assert(box.getElementsByTagName('linearGradient').length === 1,
       'getElementsByTagName finds the camelCase element');

// ---- paint: a gradient made with createElementNS fills the rect ---------------------
const root = document.getElementById('root');
root.innerHTML = '';
root.style.cssText = 'position:absolute;left:0;top:0;margin:0;padding:0;';

function makeScene(buildGradient) {
    const svg = document.createElementNS(SVG, 'svg');
    svg.setAttribute('width', '100');
    svg.setAttribute('height', '40');
    svg.style.display = 'block';
    const defs = document.createElementNS(SVG, 'defs');
    defs.appendChild(buildGradient());
    svg.appendChild(defs);
    const rect = document.createElementNS(SVG, 'rect');
    rect.setAttribute('width', '100');
    rect.setAttribute('height', '40');
    rect.setAttribute('fill', 'url(#g1)');
    svg.appendChild(rect);
    return svg;
}
const svgA = makeScene(() => {
    const g = document.createElementNS(SVG, 'linearGradient');
    g.setAttribute('id', 'g1');
    for (const [off, color] of [['0', '#ff0000'], ['1', '#0000ff']]) {
        const s = document.createElementNS(SVG, 'stop');
        s.setAttribute('offset', off);
        s.setAttribute('stop-color', color);
        g.appendChild(s);
    }
    return g;
});
root.appendChild(svgA);
flush();
const r = svgA.getBoundingClientRect();
const left = getPixel(r.left + 3, r.top + 20);
const right = getPixel(r.left + 96, r.top + 20);
assert(left.r > 200 && left.b < 60, 'gradient starts red at the left, got ' + JSON.stringify(left));
assert(right.b > 200 && right.r < 60, 'gradient ends blue at the right, got ' + JSON.stringify(right));

// The same scene built from markup.
root.innerHTML = '<svg width="100" height="40" style="display:block">' +
    '<defs><linearGradient id="g2"><stop offset="0" stop-color="#00ff00"/>' +
    '<stop offset="1" stop-color="#00ff00"/></linearGradient></defs>' +
    '<rect width="100" height="40" fill="url(#g2)"/></svg>';
flush();
const r2 = root.querySelector('svg').getBoundingClientRect();
const mid = getPixel(r2.left + 50, r2.top + 20);
assert(mid.g > 200 && mid.r < 60, 'parsed gradient paints, got ' + JSON.stringify(mid));

root.innerHTML = '';
console.log('createElementNS case: OK');
