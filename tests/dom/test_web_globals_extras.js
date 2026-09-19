// The web-platform surface the bronze port left out (docs/transition-drift.md
// rows E3, E5–E8, E11 and H7's createEvent / initEvent):
//   performance.mark / measure / getEntries* / clearMarks / clearMeasures / timeOrigin
//   navigator.hardwareConcurrency / languages / mediaDevices / onLine / cookieEnabled / product / productSub / vendor
//   document.hidden / visibilityState / location / URL / documentURI / implementation / createEvent
//   Event.prototype.initEvent
//   DataTransfer / CanvasRenderingContext2D / AudioDestinationNode / SVGElement / MathMLElement
//   the per-tag HTML*Element constructors
//   requestIdleCallback / cancelIdleCallback, window.screenX / screenY
//   bro.<ns>.available === true on every compiled-in namespace

// --- performance -------------------------------------------------------------
assert(typeof performance.now() === 'number', 'performance.now');
assert(typeof performance.timeOrigin === 'number' && performance.timeOrigin > 1e12, 'performance.timeOrigin is a wall-clock ms epoch');
performance.clearMarks();
performance.clearMeasures();
const t0 = performance.now();
const m1 = performance.mark('a');
assert(m1 && m1.entryType === 'mark' && m1.name === 'a', 'mark() returns the entry');
assert(m1.startTime === t0, 'a mark is stamped on the rAF clock (frozen within a step)');
advanceTime(100);
performance.mark('b');
const meas = performance.measure('a-b', 'a', 'b');
assert(meas.entryType === 'measure' && Math.abs(meas.duration - 100) < 1e-6,
       'measure between marks spans the virtual 100ms, got ' + meas.duration);
assert(performance.getEntriesByType('mark').length === 2, 'two marks');
assert(performance.getEntriesByName('a-b').length === 1, 'getEntriesByName finds the measure');
assert(performance.getEntriesByName('a', 'mark').length === 1, 'getEntriesByName with a type');
assert(performance.getEntries().length === 3, 'getEntries lists all three');
const objMeas = performance.measure('obj', { start: 'a', duration: 30 });
assert(Math.abs(objMeas.duration - 30) < 1e-6 && objMeas.startTime === t0, 'measure() options form');
let threw = false;
try { performance.measure('bad', 'nope'); } catch (e) { threw = true; }
assert(threw, 'measure with an unknown mark throws');
performance.clearMarks('a');
assert(performance.getEntriesByType('mark').length === 1, 'clearMarks(name) removes one');
performance.clearMarks();
performance.clearMeasures();
assert(performance.getEntries().length === 0, 'clearMarks()/clearMeasures() empty the list');
assert(typeof m1.toJSON().name === 'string', 'entries serialize');

// --- navigator ----------------------------------------------------------------
assert(Number.isInteger(navigator.hardwareConcurrency) && navigator.hardwareConcurrency >= 1, 'hardwareConcurrency');
assert(Array.isArray(navigator.languages) && navigator.languages[0] === 'en-US', 'languages');
assert(navigator.onLine === true, 'onLine');
assert(navigator.cookieEnabled === false, 'cookieEnabled');
assert(navigator.product === 'Gecko' && navigator.productSub === '20030107' && navigator.vendor === '', 'product/productSub/vendor');
assert(typeof navigator.mediaDevices === 'object' && typeof navigator.mediaDevices.enumerateDevices === 'function', 'mediaDevices');
let devices = null;
navigator.mediaDevices.enumerateDevices().then(d => { devices = d; });
flush(); advanceTime(1); flush();
assert(Array.isArray(devices) && devices.length === 0, 'enumerateDevices resolves empty');

// --- document identity / visibility -----------------------------------------
assert(document.hidden === false, 'headless document is visible');
assert(document.visibilityState === 'visible', 'visibilityState');
assert(document.location === window.location, 'document.location is window.location');
assert(document.URL === location.href && document.documentURI === location.href, 'URL / documentURI follow location.href');
assert(document.compatMode === 'CSS1Compat' && document.characterSet === 'UTF-8', 'compatMode / characterSet');
assert(typeof document.implementation === 'object', 'document.implementation');
assert(document.implementation.hasFeature() === true, 'hasFeature');
const hd = document.implementation.createHTMLDocument('Made');
assert(hd && hd !== document, 'createHTMLDocument returns a new document');
assert(hd.title === 'Made', 'createHTMLDocument sets the title, got ' + hd.title);
assert(hd.body && hd.body.tagName.toLowerCase() === 'body', 'the new document has a body');
const el = hd.createElement('div');
hd.body.appendChild(el);
assert(hd.querySelector('div') === el, 'the new document is queryable');

// --- createEvent / initEvent ----------------------------------------------------
const ce = document.createEvent('Event');
assert(ce instanceof Event, 'createEvent("Event") yields an Event');
assert(typeof ce.initEvent === 'function', 'initEvent exists');
ce.initEvent('ping', true, true);
assert(ce.type === 'ping' && ce.bubbles === true && ce.cancelable === true, 'initEvent sets type/bubbles/cancelable');
let got = 0;
const target = document.createElement('div');
document.body.appendChild(target);
target.addEventListener('ping', (e) => { got++; e.preventDefault(); });
const notCancelled = target.dispatchEvent(ce);
assert(got === 1 && notCancelled === false, 'a createEvent()/initEvent() event dispatches and can be cancelled');
ce.initEvent('ping', false, false);
assert(ce.defaultPrevented === false, 'initEvent resets defaultPrevented');
const cu = document.createEvent('CustomEvent');
assert(cu instanceof CustomEvent, 'createEvent("CustomEvent") yields a CustomEvent');
assert(document.createEvent('MouseEvents') instanceof Event, 'legacy plural names accepted');
threw = false;
try { document.createEvent('NoSuchThing'); } catch (e) { threw = String(e).indexOf('NotSupportedError') !== -1; }
assert(threw, 'unknown interface throws NotSupportedError');

// --- globals / brands ------------------------------------------------------------
for (const name of ['DataTransfer', 'CanvasRenderingContext2D', 'AudioDestinationNode', 'SVGElement', 'MathMLElement']) {
    assert(typeof globalThis[name] === 'function', name + ' is a global');
}
const cv = document.createElement('canvas');
assert(cv.getContext('2d') instanceof CanvasRenderingContext2D, '2d context instanceof CanvasRenderingContext2D');
const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
assert(svg instanceof SVGElement && svg instanceof Element && !(svg instanceof HTMLElement), '<svg> instanceof SVGElement, not HTMLElement');
const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
assert(circle instanceof SVGElement, '<circle> instanceof SVGElement');
assert(document.createElement('math') instanceof MathMLElement, '<math> instanceof MathMLElement');

const tagMap = {
    label: 'HTMLLabelElement', br: 'HTMLBRElement', hr: 'HTMLHRElement', pre: 'HTMLPreElement',
    ol: 'HTMLOListElement', dl: 'HTMLDListElement', details: 'HTMLDetailsElement', dialog: 'HTMLDialogElement',
    meta: 'HTMLMetaElement', link: 'HTMLLinkElement', title: 'HTMLTitleElement', head: 'HTMLHeadElement',
    fieldset: 'HTMLFieldSetElement', legend: 'HTMLLegendElement', progress: 'HTMLProgressElement',
    meter: 'HTMLMeterElement', output: 'HTMLOutputElement', slot: 'HTMLSlotElement', source: 'HTMLSourceElement',
    track: 'HTMLTrackElement', time: 'HTMLTimeElement', data: 'HTMLDataElement', datalist: 'HTMLDataListElement',
    optgroup: 'HTMLOptGroupElement', picture: 'HTMLPictureElement', embed: 'HTMLEmbedElement', object: 'HTMLObjectElement',
    map: 'HTMLMapElement', area: 'HTMLAreaElement', menu: 'HTMLMenuElement', base: 'HTMLBaseElement',
    caption: 'HTMLTableCaptionElement', col: 'HTMLTableColElement', colgroup: 'HTMLTableColElement',
    thead: 'HTMLTableSectionElement', tbody: 'HTMLTableSectionElement', tfoot: 'HTMLTableSectionElement',
    del: 'HTMLModElement', ins: 'HTMLModElement', blockquote: 'HTMLQuoteElement', q: 'HTMLQuoteElement',
};
for (const tag in tagMap) {
    const ctor = globalThis[tagMap[tag]];
    assert(typeof ctor === 'function', tagMap[tag] + ' is a global');
    const e = document.createElement(tag);
    assert(e instanceof ctor, '<' + tag + '> instanceof ' + tagMap[tag]);
    assert(e instanceof HTMLElement, '<' + tag + '> instanceof HTMLElement');
    assert(e.constructor.name === tagMap[tag], '<' + tag + '>.constructor.name is ' + tagMap[tag] + ', got ' + e.constructor.name);
}
assert(!(document.createElement('div') instanceof HTMLLabelElement), 'div is not a label');
threw = false;
try { new HTMLLabelElement(); } catch (e) { threw = true; }
assert(threw, 'per-tag constructors are not constructible');

// --- requestIdleCallback ---------------------------------------------------------
let idleRan = 0, deadline = null;
const idleId = requestIdleCallback((d) => { idleRan++; deadline = d; });
assert(typeof idleId === 'number', 'requestIdleCallback returns an id');
const cancelled = requestIdleCallback(() => { idleRan += 100; });
cancelIdleCallback(cancelled);
advanceTime(16);
flush();
assert(idleRan === 1, 'idle callback ran once on the next tick, got ' + idleRan);
assert(deadline && deadline.didTimeout === false && typeof deadline.timeRemaining === 'function', 'IdleDeadline shape');
assert(deadline.timeRemaining() >= 0 && deadline.timeRemaining() <= 50, 'timeRemaining within the 50ms budget');
let timedOut = null;
requestIdleCallback((d) => { timedOut = d.didTimeout; }, { timeout: 5 });
advanceTime(20);
flush();
assert(timedOut === true, 'didTimeout reports a callback that ran past its timeout');

// --- window.screenX / screenY --------------------------------------------------------
assert(window.screenX === 0 && window.screenY === 0, 'headless screenX/screenY are 0');
assert(screenLeft === 0 && screenTop === 0, 'screenLeft/screenTop aliases');

// --- bro.<ns>.available ------------------------------------------------------------------
const GATED = ['tts', 'lm', 'stt', 'diar', 'net', 'ai', 'gesture', 'gizmo', 'impostor', 'kws', 'listen',
               'motion', 'rave', 'sense', 'triposplat', 'vision', 'wake', 'diffusion', 'tensor', 'mesh', 'scene'];
for (const ns of GATED) {
    const o = bro[ns];
    assert(o && typeof o === 'object', 'bro.' + ns + ' exists');
    assert(o.available === true || o.available === false, 'bro.' + ns + '.available is a boolean, got ' + o.available);
    if (o.available === true) {
        assert(Object.keys(o).length > 1, 'bro.' + ns + ' with available:true carries members');
    }
}
assert(Physics.available === true || Physics.available === false, 'Physics.available is a boolean');

console.log('web globals extras OK');
