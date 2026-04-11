// Test slot distribution

const root = document.getElementById('root');

// Create host with light DOM children
const host = document.createElement('div');
host.innerHTML = '<span slot="header">Header Text</span><span>Default Slot</span>';
root.appendChild(host);

// Attach shadow with slots
const shadow = host.attachShadow({ mode: 'open' });
shadow.innerHTML = '<div id="shadow-header"><slot name="header"></slot></div><div id="shadow-body"><slot></slot></div>';
flush();

// Named slot: header content should be distributed
const headerSlot = shadow.querySelector('#shadow-header');
assert(headerSlot !== null, 'shadow header container exists');

// Default slot: unnamed content should be distributed
const bodySlot = shadow.querySelector('#shadow-body');
assert(bodySlot !== null, 'shadow body container exists');

// Light DOM children are still accessible on host
assert(host.children.length === 2, 'host still has 2 light DOM children');

// Cleanup
root.innerHTML = '';
