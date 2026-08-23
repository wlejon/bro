// C++ event listeners: window and element, alongside the JS ones.
//
// An AOT-compiled app has no app.js. `window.addEventListener('resize', …)`
// and `canvas.addEventListener('click', …)` in its source have to lower onto
// something in C++, and that something must reach the SAME dispatch the JS
// listeners reach — not a parallel simpler path that skips capture, bubbling,
// shadow retargeting or cancellation.
//
// This is the test for that API:
//   bro::engine::Engine::addWindowEventListener / removeWindowEventListener
//   bro::dom::Element::addEventListener(type, std::function<void(Event&)>, opts)
//                    / removeEventListener(ListenerHandle)
//
// It is driven through `__host` (src/headless/main.cpp) — the smallest
// possible host application, built out of nothing but the public engine
// surface, which is the shape host-generated C++ has. `__host.note(tag)`
// appends to the very same log the C++ listeners write to, so the relative
// order of a C++ and a JS listener is directly observable.
//
// What is asserted, in order:
//   1. a C++ window listener fires for a real resize
//   2. it receives a real dom::Event (type / isTrusted / bubbles / cancelable)
//   3. removal actually removes
//   4. a C++ element listener fires for a click delivered by the input
//      pipeline, with the right target/currentTarget/phase and mouse coords
//   5. C++ and JS listeners on one target fire in REGISTRATION order across
//      both kinds — not "all C++ then all JS"
//   6. capture and bubble phases put C++ listeners in the right place in the
//      path, and shadow retargeting is applied to them too
//   7. preventDefault() from C++ is visible to the JS listeners after it and
//      suppresses the default action
//   8. stopPropagation() from C++ ends the walk up the tree
//   9. stopImmediatePropagation() from C++ stops the rest of THIS target
//  10. `once` unregisters after one call
//  11. window listeners fire for JS-dispatched window events too

const root = document.getElementById('root');

function log() { return __host.log(); }
function reset() { __host.clearLog(); }
function seq() { return log().join(','); }

// ---------------------------------------------------------------------------
// 1-3. window resize
// ---------------------------------------------------------------------------
reset();
const winId = __host.addWindowListener('resize', 'cpp-resize');
assert(winId > 0, 'addWindowEventListener returned a handle');

resize(900, 640);
flush();
assert(seq() === 'cpp-resize', 'C++ window listener fired for resize, once: ' + seq());

const ev = __host.lastEvent();
assert(ev !== null, 'C++ listener saw an event object');
assert(ev.type === 'resize', 'event type is resize, got ' + ev.type);
assert(ev.isTrusted === true, 'engine-generated resize is trusted');
assert(ev.bubbles === false, 'resize does not bubble');
assert(ev.cancelable === false, 'resize is not cancelable');
// window is not an Element, so there is no element target for a window event.
assert(ev.target === '', 'window event has no element target, got "' + ev.target + '"');

// The size itself is read from the engine, exactly as a JS listener reads
// window.innerWidth — the resize event carries no dimensions on either side.
assert(window.innerWidth === 900, 'innerWidth updated for the resize');

reset();
assert(__host.removeWindowListener(winId) === true, 'removeWindowEventListener found it');
assert(__host.removeWindowListener(winId) === false, 'second removal reports nothing removed');
resize(880, 620);
flush();
assert(seq() === '', 'removed C++ window listener did not fire: ' + seq());

// ---------------------------------------------------------------------------
// 4. element listener, driven by real input
// ---------------------------------------------------------------------------
root.innerHTML =
    '<div id="outer" style="position:absolute;left:0;top:0;width:200px;height:120px;">' +
    '<div id="inner" style="position:absolute;left:20px;top:20px;width:80px;height:40px;"></div>' +
    '</div>';
flush();

const outer = document.getElementById('outer');
const inner = document.getElementById('inner');

reset();
const clickId = __host.addElementListener(inner, 'click', 'cpp-click');
click(40, 30);                       // inside #inner
assert(seq() === 'cpp-click', 'C++ element listener fired for a real click: ' + seq());

const ce = __host.lastEvent();
assert(ce.type === 'click', 'C++ listener got the click event');
assert(ce.target === '#inner', 'target is the clicked element, got ' + ce.target);
assert(ce.currentTarget === '#inner', 'currentTarget is the listening element');
assert(ce.eventPhase === 2, 'AT_TARGET phase, got ' + ce.eventPhase);
assert(ce.isTrusted === true, 'input-generated click is trusted');
assert(ce.clientX === 40 && ce.clientY === 30,
       'MouseEvent coordinates reached the C++ listener: ' + ce.clientX + ',' + ce.clientY);

assert(__host.removeElementListener(inner, clickId) === true, 'element listener removed');
reset();
click(40, 30);
assert(seq() === '', 'removed C++ element listener did not fire: ' + seq());

// ---------------------------------------------------------------------------
// 5. registration order across C++ and JS on one target
// ---------------------------------------------------------------------------
reset();
const jsFirst = () => __host.note('js-1');
const jsLast = () => __host.note('js-2');

inner.addEventListener('click', jsFirst);          // 1st registration
const mid = __host.addElementListener(inner, 'click', 'cpp-mid');  // 2nd
inner.addEventListener('click', jsLast);           // 3rd

click(40, 30);
assert(seq() === 'js-1,cpp-mid,js-2',
       'C++ and JS listeners fire in registration order, got: ' + seq());

inner.removeEventListener('click', jsFirst);
inner.removeEventListener('click', jsLast);
__host.removeElementListener(inner, mid);

// ---------------------------------------------------------------------------
// 6. capture / bubble / at-target, and the composed path
// ---------------------------------------------------------------------------
reset();
const capId = __host.addElementListener(outer, 'click', 'cpp-outer-capture', { capture: true });
const tgtId = __host.addElementListener(inner, 'click', 'cpp-inner');
const bubId = __host.addElementListener(outer, 'click', 'cpp-outer-bubble');

click(40, 30);
assert(seq() === 'cpp-outer-capture,cpp-inner,cpp-outer-bubble',
       'C++ listeners run in the real capture/target/bubble walk, got: ' + seq());

// The last one to run was #outer's bubble listener: currentTarget moved with
// the walk while target stayed on the element that was hit.
const be = __host.lastEvent();
assert(be.currentTarget === '#outer', 'currentTarget follows the path, got ' + be.currentTarget);
assert(be.target === '#inner', 'target stays the hit element, got ' + be.target);
assert(be.eventPhase === 3, 'BUBBLING_PHASE at #outer, got ' + be.eventPhase);

__host.removeElementListener(outer, capId);
__host.removeElementListener(inner, tgtId);
__host.removeElementListener(outer, bubId);

// Shadow retargeting: a listener OUTSIDE the shadow tree must see the host as
// the target, not the inner node that was actually hit.
root.innerHTML = '<div id="host" style="position:absolute;left:0;top:0;width:120px;height:60px;"></div>';
flush();
const shadowHost = document.getElementById('host');
const shadow = shadowHost.attachShadow({ mode: 'open' });
shadow.innerHTML = '<div id="shadowchild" style="width:120px;height:60px;"></div>';
flush();

reset();
const hostId = __host.addElementListener(shadowHost, 'click', 'cpp-host');
click(30, 20);
assert(seq() === 'cpp-host', 'C++ listener on the shadow host fired: ' + seq());
const se = __host.lastEvent();
assert(se.target === '#host',
       'C++ listener outside the shadow tree sees the retargeted target, got ' + se.target);
__host.removeElementListener(shadowHost, hostId);

// ---------------------------------------------------------------------------
// 7. preventDefault from C++
// ---------------------------------------------------------------------------
// <summary> toggling its <details> is a real default action: the engine runs
// it only when the click event was not cancelled (replaced_elements.cpp), so
// it shows preventDefault() from C++ reaching all the way through.
root.innerHTML =
    '<details id="det" style="position:absolute;left:0;top:0;width:200px;">' +
    '<summary id="sum" style="height:24px;">summary</summary>body</details>';
flush();
const det = document.getElementById('det');
const sum = document.getElementById('sum');
const sumRect = sum.getBoundingClientRect();
const sumX = sumRect.x + sumRect.width / 2;
const sumY = sumRect.y + sumRect.height / 2;

// Baseline: without a C++ listener, the click opens the <details>.
assert(det.hasAttribute('open') === false, 'details starts closed');
click(sumX, sumY);
assert(det.hasAttribute('open') === true, 'a plain click opens the details');
det.removeAttribute('open');
flush();

reset();
let sawPreventedInJs = null;
const jsAfter = (e) => { sawPreventedInJs = e.defaultPrevented; __host.note('js-after'); };
const pdId = __host.addElementListener(sum, 'click', 'cpp-prevent', { preventDefault: true });
sum.addEventListener('click', jsAfter);   // registered after → runs after

click(sumX, sumY);
assert(seq() === 'cpp-prevent,js-after', 'both ran, C++ first: ' + seq());
assert(sawPreventedInJs === true,
       'preventDefault() from C++ is visible to the JS listener after it');
assert(det.hasAttribute('open') === false,
       'preventDefault() from C++ suppressed the <details> default action');

sum.removeEventListener('click', jsAfter);
__host.removeElementListener(sum, pdId);

// ---------------------------------------------------------------------------
// 8. stopPropagation from C++
// ---------------------------------------------------------------------------
root.innerHTML =
    '<div id="outer2" style="position:absolute;left:0;top:0;width:200px;height:120px;">' +
    '<div id="inner2" style="position:absolute;left:20px;top:20px;width:80px;height:40px;"></div>' +
    '</div>';
flush();
const outer2 = document.getElementById('outer2');
const inner2 = document.getElementById('inner2');

reset();
const stopId = __host.addElementListener(inner2, 'click', 'cpp-stop', { stopPropagation: true });
const jsAncestor = () => __host.note('js-ancestor');
const jsWindow = () => __host.note('js-window');
outer2.addEventListener('click', jsAncestor);
window.addEventListener('click', jsWindow);

click(40, 30);
assert(seq() === 'cpp-stop',
       'stopPropagation() from C++ stopped the walk before the ancestor and window: ' + seq());
__host.removeElementListener(inner2, stopId);

// Without it, both would have run — otherwise the assertion above proves
// nothing about stopPropagation.
reset();
click(40, 30);
assert(seq() === 'js-ancestor,js-window',
       'the ancestor and window listeners do run when nothing stops them: ' + seq());
outer2.removeEventListener('click', jsAncestor);
window.removeEventListener('click', jsWindow);

// ---------------------------------------------------------------------------
// 9. stopImmediatePropagation from C++
// ---------------------------------------------------------------------------
reset();
const immId = __host.addElementListener(inner2, 'click', 'cpp-imm',
                                        { stopImmediatePropagation: true });
const jsSameTarget = () => __host.note('js-same-target');
inner2.addEventListener('click', jsSameTarget);   // after it → must not run
click(40, 30);
assert(seq() === 'cpp-imm',
       'stopImmediatePropagation() from C++ stopped the rest of this target too: ' + seq());
__host.removeElementListener(inner2, immId);
inner2.removeEventListener('click', jsSameTarget);

// ---------------------------------------------------------------------------
// 10. once
// ---------------------------------------------------------------------------
reset();
__host.addElementListener(inner2, 'click', 'cpp-once', { once: true });
click(40, 30);
click(40, 30);
assert(seq() === 'cpp-once', 'a once listener ran exactly once: ' + seq());

reset();
__host.addWindowListener('resize', 'cpp-win-once', { once: true });
resize(870, 610);
flush();
resize(860, 600);
flush();
assert(seq() === 'cpp-win-once', 'a once window listener ran exactly once: ' + seq());

// ---------------------------------------------------------------------------
// 11. JS-dispatched window events reach C++ listeners too
// ---------------------------------------------------------------------------
reset();
const customId = __host.addWindowListener('appsignal', 'cpp-custom');
window.addEventListener('appsignal', () => __host.note('js-custom'));
window.dispatchEvent(new CustomEvent('appsignal', { detail: 7 }));
assert(seq() === 'cpp-custom,js-custom',
       'window.dispatchEvent reached the C++ listener, in registration order: ' + seq());
__host.removeWindowListener(customId);

// A JS-dispatched resize goes through the same door as the engine's.
reset();
const jsResizeId = __host.addWindowListener('resize', 'cpp-jsresize');
window.dispatchEvent(new Event('resize'));
assert(seq() === 'cpp-jsresize', 'JS-dispatched resize reached the C++ listener: ' + seq());
assert(__host.lastEvent().isTrusted === false,
       'a JS-dispatched event is not trusted on the C++ side either');
__host.removeWindowListener(jsResizeId);

root.innerHTML = '';
reset();
