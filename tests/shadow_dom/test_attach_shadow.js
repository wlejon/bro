// Test attachShadow and shadow DOM basics

const root = document.getElementById('root');
const host = document.createElement('div');
host.id = 'host';
root.appendChild(host);

// Attach shadow
const shadow = host.attachShadow({ mode: 'open' });
assert(shadow !== null, 'attachShadow returns shadow root');
assert(host.shadowRoot === shadow, 'host.shadowRoot returns shadow');

// Add content to shadow root
const inner = document.createElement('span');
inner.textContent = 'shadow content';
shadow.appendChild(inner);

// Shadow content is not in light DOM children
assert(host.children.length === 0, 'light DOM has no children');

// But shadow root has children
assert(shadow.childNodes.length > 0, 'shadow root has child nodes');

// querySelector inside shadow
const found = shadow.querySelector('span');
assert(found === inner, 'querySelector in shadow finds element');

// Cleanup
root.innerHTML = '';
