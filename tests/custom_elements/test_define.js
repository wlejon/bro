// Test customElements.define and basic lifecycle

let constructed = false;
let connected = false;
let disconnected = false;

class MyElement extends HTMLElement {
    constructor() {
        super();
        constructed = true;
    }
    connectedCallback() {
        connected = true;
    }
    disconnectedCallback() {
        disconnected = true;
    }
}

customElements.define('my-element', MyElement);

const root = document.getElementById('root');

// Create via document.createElement
const el = document.createElement('my-element');
assert(constructed, 'constructor called on createElement');

// Append to DOM triggers connectedCallback
root.appendChild(el);
flush();
assert(connected, 'connectedCallback called on appendChild');

// Remove from DOM triggers disconnectedCallback
root.removeChild(el);
flush();
assert(disconnected, 'disconnectedCallback called on removeChild');

// Cleanup
root.innerHTML = '';
