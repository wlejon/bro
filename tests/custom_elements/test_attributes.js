// Test custom element observedAttributes and attributeChangedCallback

const changes = [];

class AttrElement extends HTMLElement {
    static get observedAttributes() {
        return ['data-value', 'data-name'];
    }
    attributeChangedCallback(name, oldValue, newValue) {
        changes.push({ name, oldValue, newValue });
    }
}

customElements.define('attr-element', AttrElement);

const root = document.getElementById('root');
const el = document.createElement('attr-element');
root.appendChild(el);
flush();

// Set observed attribute
el.setAttribute('data-value', 'hello');
assert(changes.length >= 1, 'attributeChangedCallback called');
assert(changes[changes.length - 1].name === 'data-value', 'correct attribute name');
assert(changes[changes.length - 1].newValue === 'hello', 'correct new value');

// Change attribute
el.setAttribute('data-value', 'world');
assert(changes.length >= 2, 'called again on change');
assert(changes[changes.length - 1].oldValue === 'hello', 'old value is hello');
assert(changes[changes.length - 1].newValue === 'world', 'new value is world');

// Non-observed attribute should not trigger callback
const countBefore = changes.length;
el.setAttribute('data-other', 'ignored');
assert(changes.length === countBefore, 'non-observed attribute does not trigger callback');

// Cleanup
root.innerHTML = '';
