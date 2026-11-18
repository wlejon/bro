// Object.prototype.toString names the interface, as in a browser
// (Symbol.toStringTag on every interface prototype and the singletons).
const tag = (v) => Object.prototype.toString.call(v);
const canvas = document.createElement('canvas');
const cases = [
    [document.body, 'HTMLBodyElement'], [document, 'HTMLDocument'], [document.createTextNode(''), 'Text'],
    [document.createDocumentFragment(), 'DocumentFragment'], [new Event('x'), 'Event'], [new MouseEvent('click'), 'MouseEvent'],
    [document.body.childNodes, 'NodeList'], [document.body.children, 'HTMLCollection'], [location, 'Location'],
    [navigator, 'Navigator'], [history, 'History'], [performance, 'Performance'], [new Range(), 'Range'],
    [canvas, 'HTMLCanvasElement'], [canvas.getContext('2d'), 'CanvasRenderingContext2D'], [new Blob([]), 'Blob'],
    [new URL('http://a/'), 'URL'], [new AbortController(), 'AbortController'], [new DOMParser(), 'DOMParser'],
    [HTMLElement.prototype, 'HTMLElement'], [new MutationObserver(() => {}), 'MutationObserver'],
    [document.createElement('div'), 'HTMLDivElement'], [new CSSStyleSheet(), 'CSSStyleSheet'],
];
for (const [v, name] of cases) assert(tag(v) === `[object ${name}]`, `${name}: ${tag(v)}`);
assert(new DOMParser() instanceof DOMParser, 'a DOMParser is an instance of DOMParser');
assert(tag(new Map()) === '[object Map]' && tag([]) === '[object Array]', 'the ECMAScript tags are their own');
console.log('test_interface_tags: OK');
