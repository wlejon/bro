// Built-in HTML element interface objects: HTMLCanvasElement and friends.
//
// bro backs every tag with one C++ Element class, so the JS-visible interface
// hierarchy has to be layered on at wrap time. Libraries branch on it —
// three.js decides a texture is serializable by asking
// `image instanceof HTMLCanvasElement`, and drops it when the guard throws or
// answers false.

// --- interface objects exist ----------------------------------------------
var names = ['HTMLElement', 'HTMLCanvasElement', 'HTMLImageElement', 'HTMLDivElement',
             'HTMLInputElement', 'HTMLSelectElement', 'HTMLTextAreaElement',
             'HTMLButtonElement', 'HTMLScriptElement', 'HTMLStyleElement',
             'HTMLAnchorElement', 'HTMLSpanElement', 'HTMLParagraphElement',
             'HTMLTableElement', 'HTMLTableRowElement', 'HTMLTableCellElement',
             'HTMLUListElement', 'HTMLLIElement', 'HTMLFormElement',
             'HTMLIFrameElement', 'HTMLVideoElement', 'HTMLAudioElement',
             'HTMLMediaElement', 'HTMLHeadingElement', 'HTMLOptionElement'];
for (var i = 0; i < names.length; i++) {
    assert(typeof globalThis[names[i]] === 'function', names[i] + ' is defined');
}

// --- instanceof, up the whole chain ---------------------------------------
var cv = document.createElement('canvas');
assert(cv instanceof HTMLCanvasElement, 'canvas instanceof HTMLCanvasElement');
assert(cv instanceof HTMLElement,       'canvas instanceof HTMLElement');
assert(cv instanceof Element,           'canvas instanceof Element');
assert(cv instanceof Node,              'canvas instanceof Node');
assert(!(cv instanceof HTMLDivElement), 'canvas is not an HTMLDivElement');

var div = document.createElement('div');
assert(div instanceof HTMLDivElement, 'div instanceof HTMLDivElement');
assert(div instanceof HTMLElement,    'div instanceof HTMLElement');
assert(!(div instanceof HTMLCanvasElement), 'div is not an HTMLCanvasElement');

var img = document.createElement('img');
assert(img instanceof HTMLImageElement, 'img instanceof HTMLImageElement');
assert(img instanceof HTMLElement,      'img instanceof HTMLElement');

// `new Image()` is an HTMLImageElement on the web; bro's decode helper chains
// onto the same prototype so library guards accept both spellings.
assert(new Image() instanceof HTMLImageElement, 'new Image() instanceof HTMLImageElement');

// Media elements chain through HTMLMediaElement.
var vid = document.createElement('video');
assert(vid instanceof HTMLVideoElement,  'video instanceof HTMLVideoElement');
assert(vid instanceof HTMLMediaElement,  'video instanceof HTMLMediaElement');
assert(vid instanceof HTMLElement,       'video instanceof HTMLElement');
var aud = document.createElement('audio');
assert(aud instanceof HTMLMediaElement,  'audio instanceof HTMLMediaElement');
assert(!(aud instanceof HTMLVideoElement), 'audio is not an HTMLVideoElement');

// Several tags can share one interface.
var td = document.createElement('td');
var th = document.createElement('th');
assert(td instanceof HTMLTableCellElement, 'td instanceof HTMLTableCellElement');
assert(th instanceof HTMLTableCellElement, 'th instanceof HTMLTableCellElement');
var h1 = document.createElement('h1');
var h3 = document.createElement('h3');
assert(h1 instanceof HTMLHeadingElement, 'h1 instanceof HTMLHeadingElement');
assert(h3 instanceof HTMLHeadingElement, 'h3 instanceof HTMLHeadingElement');

// A tag with no dedicated interface still lands on HTMLElement.
var section = document.createElement('section');
assert(section instanceof HTMLElement, 'section instanceof HTMLElement');
assert(section instanceof Element,     'section instanceof Element');

// --- parsed elements, not just created ones -------------------------------
var host = document.createElement('div');
host.innerHTML = '<canvas id="parsed-cv"></canvas><p id="parsed-p">hi</p>' +
                 '<input id="parsed-in"><select id="parsed-sel"></select>';
document.body.appendChild(host);
flush();
assert(document.getElementById('parsed-cv') instanceof HTMLCanvasElement,
       'parsed canvas instanceof HTMLCanvasElement');
assert(document.getElementById('parsed-p') instanceof HTMLParagraphElement,
       'parsed p instanceof HTMLParagraphElement');
assert(document.getElementById('parsed-in') instanceof HTMLInputElement,
       'parsed input instanceof HTMLInputElement');
assert(document.getElementById('parsed-sel') instanceof HTMLSelectElement,
       'parsed select instanceof HTMLSelectElement');

// document.body / documentElement come from the parser too.
assert(document.body instanceof HTMLBodyElement, 'document.body instanceof HTMLBodyElement');
assert(document.documentElement instanceof HTMLHtmlElement,
       'documentElement instanceof HTMLHtmlElement');

// --- constructor identity --------------------------------------------------
assert(cv.constructor === HTMLCanvasElement, 'canvas.constructor is HTMLCanvasElement');
assert(cv.constructor.name === 'HTMLCanvasElement', 'constructor is named');
assert(Object.getPrototypeOf(cv) === HTMLCanvasElement.prototype,
       'canvas prototype is HTMLCanvasElement.prototype');

// Interface objects are not callable — `new HTMLCanvasElement()` is a TypeError.
var threw = false;
try { new HTMLCanvasElement(); } catch (e) { threw = true; }
assert(threw, 'new HTMLCanvasElement() throws (illegal constructor)');

// --- the methods still resolve through the longer chain -------------------
assert(typeof cv.getContext === 'function', 'getContext still reachable on canvas');
assert(typeof cv.toDataURL === 'function', 'toDataURL still reachable on canvas');
assert(typeof div.appendChild === 'function', 'appendChild still reachable on div');
assert(typeof div.setAttribute === 'function', 'setAttribute still reachable on div');
cv.width = 8; cv.height = 8;
document.body.appendChild(cv);
assert(cv.getContext('2d') !== null, 'getContext works after the prototype swap');

// The wrapper is cached, so identity and the prototype must survive a re-wrap.
var again = document.getElementById('parsed-cv');
assert(again instanceof HTMLCanvasElement, 're-fetched element keeps its interface');

console.log('PASS: HTML element interfaces');
