// An element obtained BEFORE customElements.define keeps its identity across
// the upgrade: the HTMLElement constructor returns the wrapper the app
// already holds, super() continues on it (ECMA-262 13.3.7.1), and the class
// prototype lands on that same object. HTML §4.13.5 "upgrades".

const root = document.getElementById('root');
root.innerHTML = '<late-one id="a" label="x"></late-one><late-one id="b"></late-one>';

const before = document.getElementById('a');
const beforeB = root.querySelector('#b');
assert(!('greet' in before), 'not yet upgraded: no class method');
before.stash = 'kept';

let seen = [];
class LateOne extends HTMLElement {
    count = 0;
    constructor() {
        super();
        this.count++;
        seen.push(this);
    }
    greet() { return 'hi ' + this.getAttribute('label'); }
    static get observedAttributes() { return ['label']; }
    attributeChangedCallback(name, oldV, newV) { this.lastAttr = name + '=' + newV; }
    connectedCallback() { this.wasConnected = true; }
}
customElements.define('late-one', LateOne);

// Identity: the same object, now an instance of the class.
assert(document.getElementById('a') === before, 'wrapper identity survives define');
assert(before instanceof LateOne, 'upgraded wrapper is an instance of the class');
assert(before instanceof HTMLElement, 'and still an HTMLElement');
assert(Object.getPrototypeOf(before) === LateOne.prototype, 'prototype is the class prototype');
assert(before.stash === 'kept', 'own properties set before the upgrade survive');

// The constructor ran on that object: field initializers and body alike.
assert(seen.length === 2, 'constructor ran once per parsed element, got ' + seen.length);
assert(seen[0] === before && seen[1] === beforeB, 'constructor `this` is the pre-existing wrapper');
assert(before.count === 1, 'field initializer then body ran on the wrapper');
assert(before.greet() === 'hi x', 'class methods resolve on the upgraded wrapper');
assert(before.lastAttr === 'label=x', 'present observed attributes reported on upgrade');
assert(before.wasConnected === true, 'connectedCallback fired for a connected element');
assert(beforeB.lastAttr === undefined, 'no attribute callback without the attribute');

// Elements created after define go through the same door.
const later = document.createElement('late-one');
assert(later instanceof LateOne && later.count === 1, 'createElement constructs the class');
later.setAttribute('label', 'y');
assert(later.greet() === 'hi y' && later.lastAttr === 'label=y', 'attribute callback after create');

// The element's DOM identity is intact: attributes and tree position.
assert(before.id === 'a' && before.parentNode === root, 'DOM state unchanged by the upgrade');

root.innerHTML = '';
