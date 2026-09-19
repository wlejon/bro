// `new MyElement()` on a DEFINED custom element.
//
// The upgrade path — createElement / innerHTML calling the registered
// constructor — has always worked. The other direction is the one every
// component library uses to build a tree it then appends:
//
//     const card = new MyCard();
//     card.title = 'x';
//     container.appendChild(card);
//
// Regression: the port's HTMLElement constructor could only finish the
// upgrade it was already inside, so a direct `new` threw "Illegal
// constructor". Now the base constructor reads the tag off the receiver's
// prototype chain, creates that element, and adopts the object `new`
// produced as its wrapper — one identity, so the instance IS the node.

const root = document.getElementById('root');

let constructions = 0;
let connections = 0;

class DriftCard extends HTMLElement {
    constructor() {
        super();
        constructions++;
        this.ownField = 'mine';
    }
    connectedCallback() { connections++; }
    describe() { return 'card:' + this.ownField; }
}
customElements.define('drift-card', DriftCard);

// --- the direct construction ---------------------------------------------
const card = new DriftCard();
assert(constructions === 1, 'the constructor ran once');
assert(card instanceof DriftCard, 'the result is an instance of the class');
assert(card instanceof HTMLElement, 'and of HTMLElement');
assert(card.tagName === 'DRIFT-CARD', 'it carries the registered tag: ' + card.tagName);
assert(card.nodeType === 1, 'it is an element node');
assert(card.ownField === 'mine', 'the constructor body ran on the same object');
assert(card.describe() === 'card:mine', 'class methods resolve on it');

// It is a real node: the Element surface works.
card.setAttribute('data-k', 'v');
assert(card.getAttribute('data-k') === 'v', 'setAttribute works on a directly built instance');
card.textContent = 'hello';
assert(card.textContent === 'hello', 'textContent works on it');
assert(card.isConnected === false, 'a freshly built instance is not connected');

// --- appending it is the whole point --------------------------------------
root.appendChild(card);
flush();
assert(card.isConnected === true, 'appending connects it');
assert(connections === 1, 'connectedCallback ran for the appended instance');
assert(root.children[0] === card,
       'the node in the tree IS the object `new` returned — one identity');
assert(document.querySelector('drift-card') === card,
       'a query for it finds the same object');
assert(constructions === 1, 'appending did not construct a second time');

root.removeChild(card);
flush();

// --- a subclass of a defined class resolves to the nearest registered one --
class DriftFancy extends DriftCard {
    constructor() { super(); this.fancy = true; }
}
const fancy = new DriftFancy();
assert(fancy instanceof DriftFancy, 'the subclass instance keeps its own class');
assert(fancy instanceof DriftCard, 'and is still a DriftCard');
assert(fancy.tagName === 'DRIFT-CARD',
       'an unregistered subclass builds the nearest registered tag: ' + fancy.tagName);
assert(fancy.fancy === true, 'the subclass constructor body ran');
assert(fancy.ownField === 'mine', 'and so did the base one');

// --- the upgrade path still works, and still produces one identity --------
const viaCreate = document.createElement('drift-card');
assert(viaCreate instanceof DriftCard, 'createElement still upgrades');
assert(viaCreate.describe() === 'card:mine', 'the upgraded element has the class surface');
root.appendChild(viaCreate);
flush();
assert(root.children[0] === viaCreate, 'and it is the node in the tree');

// --- an undefined class is still an illegal constructor -------------------
class NotDefined extends HTMLElement {}
let threw = false;
try { new NotDefined(); } catch (e) { threw = true; }
assert(threw, 'constructing a class that was never defined still throws');

let threwBase = false;
try { new HTMLElement(); } catch (e) { threwBase = true; }
assert(threwBase, 'new HTMLElement() on its own is still an illegal constructor');

root.innerHTML = '';
