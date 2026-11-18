// A ShadowRoot is an EventTarget on the composed path (DOM 2.9, Chromium):
// listeners on it hear events from inside its tree, with the root as
// currentTarget; a non-composed event stops at the root; a composed one goes
// on to the host retargeted; shadowRoot.dispatchEvent targets the root; Node
// is an EventTarget; and a slot whose assigned nodes change gets a
// `slotchange` (bubbling to the root) at the next microtask checkpoint,
// including for a child's `slot` attribute and a slot's `name`. Shoelace
// (Lit) calls shadowRoot.addEventListener('slotchange', ...) when its
// elements connect; bro threw there.

const host = document.createElement('div');
document.body.appendChild(host);
const sr = host.attachShadow({ mode: 'open' });
sr.innerHTML = '<slot></slot><slot name="named"></slot><p id="p">in shadow</p>';

assert(typeof sr.addEventListener === 'function', 'ShadowRoot has addEventListener');
assert(sr instanceof EventTarget, 'a ShadowRoot is an EventTarget');
assert(document.body instanceof EventTarget && document.createTextNode('') instanceof EventTarget,
       'elements and text nodes are EventTargets');
assert(Object.getPrototypeOf(Node.prototype) === EventTarget.prototype, 'Node.prototype inherits EventTarget');

const log = [];
sr.addEventListener('ping', (e) => log.push('root:' + (e.currentTarget === sr) + ':' + e.target.id + ':' + e.eventPhase));
host.addEventListener('ping', (e) => log.push('host:' + (e.target === host)));
const p = sr.getElementById('p');
p.dispatchEvent(new Event('ping', { bubbles: true }));
assert(log.join(' ') === 'root:true:p:3', 'a non-composed event stops at the root, got ' + log.join(' '));

log.length = 0;
p.dispatchEvent(new Event('ping', { bubbles: true, composed: true }));
assert(log.join(' ') === 'root:true:p:3 host:true', 'a composed one reaches the host retargeted, got ' + log.join(' '));

let captured = '';
sr.addEventListener('cap', () => { captured += 'root'; }, true);
p.addEventListener('cap', () => { captured += '>p'; });
p.dispatchEvent(new Event('cap'));
assert(captured === 'root>p', 'a capture listener on the root runs before the target, got ' + captured);

let atRoot = null;
sr.addEventListener('direct', (e) => { atRoot = e.target === sr && e.eventPhase === 2; });
assert(sr.dispatchEvent(new Event('direct')) === true, 'dispatchEvent on a root returns true');
assert(atRoot === true, 'shadowRoot.dispatchEvent targets the root');

let removedRan = false;
const removed = () => { removedRan = true; };
sr.addEventListener('gone', removed);
sr.removeEventListener('gone', removed);
p.dispatchEvent(new Event('gone', { bubbles: true }));
assert(!removedRan, 'removeEventListener on a root removes');

const changes = [];
sr.addEventListener('slotchange', (e) => changes.push(e.target.getAttribute('name') || 'default'));
host.appendChild(document.createElement('span'));
assert(changes.length === 0, 'slotchange is not dispatched synchronously');
await Promise.resolve();
await Promise.resolve();
assert(changes.join(',') === 'default', 'appending a child signals the default slot, got ' + changes.join(','));

changes.length = 0;
const b = document.createElement('b');
host.appendChild(b);
b.setAttribute('slot', 'named');
await new Promise((r) => setTimeout(r, 0));
assert(changes.join(',') === 'named', 'one slotchange at the slot the node ends up in, got ' + changes.join(','));

changes.length = 0;
b.removeAttribute('slot');
await new Promise((r) => setTimeout(r, 0));
assert(changes.sort().join(',') === 'default,named', 'moving a node signals both slots, got ' + changes.join(','));

changes.length = 0;
host.appendChild(document.createElement('i'));
host.lastChild.remove();
await new Promise((r) => setTimeout(r, 0));
assert(changes.length === 0, 'no slotchange when the assignment ends where it began, got ' + changes.join(','));

// A slot inside a nested shadow host's light DOM is still this tree's slot
// (Shoelace's tab group puts <slot name="nav"> inside <sl-resize-observer>).
const outer = document.createElement('div');
document.body.appendChild(outer);
const outerRoot = outer.attachShadow({ mode: 'open' });
outerRoot.innerHTML = '<section><slot name="nav"></slot></section>';
outerRoot.querySelector('section').attachShadow({ mode: 'open' }).innerHTML = '<slot></slot>';
const tab = document.createElement('span');
tab.slot = 'nav';
outer.appendChild(tab);
assert(outerRoot.querySelector('slot[name=nav]').assignedElements()[0] === tab,
       'a slot in a nested host\'s light DOM takes its node');
assert(tab.assignedSlot === outerRoot.querySelector('slot[name=nav]'), 'and assignedSlot names it');

document.body.removeChild(host);
document.body.removeChild(outer);
