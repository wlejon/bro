// What one realm does must not change what another calls (docs/app-isolation.md,
// step 2). A frame is a realm of its own, but the builtins are one set per
// thread, so a frame that patched Array.prototype would patch the page's;
// lockdown freezes them before the first script.
// And each window has its own CustomElementRegistry: a frame defining <x-iso>
// used to define it for the page, and the page could then not define its own.

const frame = document.createElement('iframe');
frame.setAttribute('src', 'realm_isolation_child');
frame.style.cssText = 'width: 200px; height: 100px';
document.body.appendChild(frame);
flush();
const cw = frame.contentWindow;
assert(cw && typeof cw.readPatch === 'function', 'the child loaded and its functions are on its contentWindow');

// The builtins: frozen, and the child's patch never landed.
assert(Object.isFrozen(Array.prototype), 'Array.prototype is frozen');
assert(Object.isFrozen(Object.prototype) && Object.isFrozen(Function.prototype), 'the root prototypes are frozen');
assert(cw.readPatch() === 'refused' || cw.readPatch() === 'threw:TypeError',
       `the child's write to Array.prototype.push was refused (${cw.readPatch()})`);
assert([].push(1) === 1, "the page's push is the real one");
let threw = null;
(() => { 'use strict'; try { Array.prototype.push = null; } catch (e) { threw = e; } })();
assert(threw instanceof TypeError, 'a strict write to a frozen builtin throws a TypeError');

// Ordinary code that assigns over an inherited builtin still works (the
// override mistake is fixed in the engine, not refused).
class MyErr extends Error {}
MyErr.prototype.name = 'MyErr';
assert(new MyErr('x').name === 'MyErr' && String(new MyErr('m')) === 'MyErr: m', 'a subclass prototype can set name');
const o = {};
o.toString = () => 'mine';
assert(String(o) === 'mine', 'an object can set its own toString');
assert(Object.prototype.toString.call(o) === '[object Object]', 'Object.prototype.toString is untouched');

// Host prototypes are the realm's own and stay extensible.
HTMLElement.prototype.pageHelper = function () { return 'helper'; };
assert(document.createElement('div').pageHelper() === 'helper', 'a host prototype is not frozen');
delete HTMLElement.prototype.pageHelper;

// Custom elements: one registry per window.
assert(cw.definedHere('x-iso') === 'function', 'the child defined <x-iso> in its own registry');
assert(customElements.get('x-iso') === undefined, "the child's definition is not in the page's registry");
class PageIso extends HTMLElement {
  constructor() { super(); this.from = 'page'; }
}
customElements.define('x-iso', PageIso);
const mine = document.createElement('x-iso');
document.body.appendChild(mine);
assert(mine.from === 'page' && mine instanceof PageIso, "the page's <x-iso> is built by the page's class");
assert(cw.makeIso() === 'child', "the child's <x-iso> is still built by the child's class");
assert(customElements.get('x-iso') === PageIso, "the page's registry answers the page's class");

console.log('test_iframe_realm_isolation OK');
