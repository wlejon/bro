// The shadow-tree surface: slot assignment, the composed root, the node name,
// and — the one with a visible pixel consequence — a <style> APPENDED into a
// shadow root rather than written with innerHTML.
//
// Regression: the port registered a shadow stylesheet only on the innerHTML
// path, so `shadow.appendChild(styleEl)` (what every component that builds its
// own tree does) left the rules unregistered and the component unstyled; and
// `slot`, `assignedSlot`, `assignedNodes`, `assignedElements` and
// `getRootNode({composed:true})` were missing or ignored their argument.

const root = document.getElementById('root');

const host = document.createElement('div');
host.id = 'sh-host';
root.appendChild(host);
host.innerHTML = '<span id="hdr" slot="header">Header</span>' +
                 '<span id="bdy">Body</span>' +
                 'loose text';

const shadow = host.attachShadow({ mode: 'open' });
shadow.innerHTML = '<slot name="header"></slot><slot></slot>';
flush();

// --- ShadowRoot.nodeName --------------------------------------------------
assert(shadow.nodeType === 11, 'a shadow root is a DocumentFragment node');
assert(shadow.nodeName === '#document-fragment',
       'nodeName for node type 11 is #document-fragment, got ' + shadow.nodeName);

// --- slot / assignedSlot / assignedNodes / assignedElements ---------------
const hdr = document.getElementById('hdr');
const bdy = document.getElementById('bdy');
assert(hdr.slot === 'header', 'slot reflects the slot attribute');
assert(bdy.slot === '', 'a child with no slot attribute reports the empty string');
bdy.slot = 'header';
assert(bdy.getAttribute('slot') === 'header', 'writing slot writes the attribute');
bdy.slot = '';
flush();

const slots = shadow.querySelectorAll('slot');
assert(slots.length === 2, 'the shadow tree has two slots: ' + slots.length);
const namedSlot = slots[0];
const defaultSlot = slots[1];
assert(namedSlot.getAttribute('name') === 'header', 'the first slot is the named one');

assert(hdr.assignedSlot === namedSlot,
       'a child with slot="header" is assigned to the named slot');
assert(bdy.assignedSlot === defaultSlot,
       'a child with no slot attribute goes to the default slot');
assert(root.assignedSlot === null,
       'an element whose parent has no shadow root has no assigned slot');

const named = namedSlot.assignedElements();
assert(named.length === 1 && named[0] === hdr,
       'assignedElements reports the elements a slot received');
const def = defaultSlot.assignedNodes();
assert(def.length >= 1, 'assignedNodes reports what the default slot received');
assert(def.indexOf(bdy) >= 0, 'assignedNodes includes the element child');
assert(defaultSlot.assignedElements().length === 1,
       'assignedElements drops the text node assignedNodes keeps: ' +
       defaultSlot.assignedElements().length);
assert(host.assignedNodes().length === 0,
       'assignedNodes on something that is not a slot is empty, not a throw');

// --- getRootNode({composed}) ---------------------------------------------
const inner = document.createElement('b');
shadow.appendChild(inner);
assert(inner.getRootNode() === shadow,
       'getRootNode() stops at the shadow root');
assert(inner.getRootNode({ composed: true }) === document,
       'getRootNode({composed:true}) steps out onto the host and reaches the document');
assert(inner.getRootNode({}) === shadow, 'an options object without composed is the default');
assert(host.getRootNode() === document, 'a light-DOM node roots at the document');

// --- isConnected crosses the shadow boundary ------------------------------
assert(inner.isConnected === true,
       'a node inside an attached shadow tree is connected');
assert(host.isConnected === true, 'the host is connected');
const loose = document.createElement('div');
assert(loose.isConnected === false, 'a created-but-unattached node is not connected');
root.appendChild(loose);
assert(loose.isConnected === true, 'appending it connects it');
root.removeChild(loose);
assert(loose.isConnected === false, 'removing it disconnects it again');

// --- a <style> APPENDED into the shadow root is registered ----------------
const styled = document.createElement('div');
styled.id = 'styled-host';
root.appendChild(styled);
const sr = styled.attachShadow({ mode: 'open' });

const target = document.createElement('p');
target.textContent = 'measure me';
sr.appendChild(target);
flush();
const plainHeight = target.getBoundingClientRect().height;

const st = document.createElement('style');
st.textContent = 'p { height: 77px; display: block; }';
sr.appendChild(st);
flush();

const styledHeight = target.getBoundingClientRect().height;
assert(Math.abs(styledHeight - 77) < 1.5,
       'the appended shadow stylesheet actually applies: ' +
       styledHeight + ' (was ' + plainHeight + ')');

// The same <style> written with innerHTML still works — the path that DID.
const sr2 = (function () {
    const h = document.createElement('div');
    root.appendChild(h);
    const s = h.attachShadow({ mode: 'open' });
    s.innerHTML = '<style>i { height: 44px; display: block; }</style><i>x</i>';
    flush();
    return s;
})();
assert(Math.abs(sr2.querySelector('i').getBoundingClientRect().height - 44) < 1.5,
       'the innerHTML shadow stylesheet applies');

// --- appending into a shadow root invalidates the slot assignment ---------
const late = document.createElement('span');
late.setAttribute('slot', 'header');
host.appendChild(late);
flush();
assert(late.assignedSlot === namedSlot,
       'a light child appended after the shadow was built still gets slotted');
host.removeChild(late);
flush();
assert(namedSlot.assignedElements().length === 1,
       'removing it takes it back out of the slot');

root.innerHTML = '';
