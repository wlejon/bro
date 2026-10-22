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

// ---- Composed event path through <slot> and ShadowRoot boundaries -----------
const slottedSpan = host.querySelector('span[slot="header"]');
const slotEl = shadow.querySelector('slot[name="header"]');
const pathLog = [];
slottedSpan.addEventListener('slot-test', (e) => pathLog.push('span:' + e.composed));
slotEl.addEventListener('slot-test', () => pathLog.push('slot'));
headerSlot.addEventListener('slot-test', () => pathLog.push('shadow-header'));
host.addEventListener('slot-test', () => pathLog.push('host'));

slottedSpan.dispatchEvent(new CustomEvent('slot-test', { bubbles: true, composed: true }));
assert(pathLog.join(',') === 'span:true,slot,shadow-header,host',
       'slotted light-DOM event traverses <slot> and shadow ancestors: ' + pathLog.join(','));

// Non-composed event originating inside shadow tree stops at ShadowRoot
const privLog = [];
slotEl.addEventListener('priv-evt', (e) => privLog.push('slot:' + e.composed));
headerSlot.addEventListener('priv-evt', () => privLog.push('shadow-header'));
host.addEventListener('priv-evt', () => privLog.push('host'));
document.addEventListener('priv-evt', () => privLog.push('doc'));
window.addEventListener('priv-evt', () => privLog.push('win'));

slotEl.dispatchEvent(new CustomEvent('priv-evt', { bubbles: true, composed: false }));
assert(privLog.join(',') === 'slot:false,shadow-header',
       'non-composed shadow event stops at ShadowRoot: ' + privLog.join(','));

// Composed event originating inside shadow tree crosses ShadowRoot to host/doc/win
const pubLog = [];
slotEl.addEventListener('pub-evt', (e) => pubLog.push('slot:' + e.composed));
headerSlot.addEventListener('pub-evt', () => pubLog.push('shadow-header'));
host.addEventListener('pub-evt', () => pubLog.push('host'));
const onDocPub = () => pubLog.push('doc');
const onWinPub = () => pubLog.push('win');
document.addEventListener('pub-evt', onDocPub);
window.addEventListener('pub-evt', onWinPub);

slotEl.dispatchEvent(new CustomEvent('pub-evt', { bubbles: true, composed: true }));
document.removeEventListener('pub-evt', onDocPub);
window.removeEventListener('pub-evt', onWinPub);
assert(pubLog.join(',') === 'slot:true,shadow-header,host,doc,win',
       'composed shadow event crosses ShadowRoot to host/doc/win: ' + pubLog.join(','));

// Cleanup
root.innerHTML = '';
