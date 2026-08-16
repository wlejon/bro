// The DOM probe: a bronze-compiled app that builds and takes apart a small
// element tree, and says what it observed at every step.
//
// It is the subject of run_dom_test.sh, and it exists because the element
// surface in src/bronze_host/host_element.cpp is the one part of this layer
// where a plausible-looking implementation can be wrong in ways no compile
// catches. Three of those ways in particular:
//
//   IDENTITY. A wrapper rebuilt on each ask would satisfy every getter here
//   and still break every UI ever written, because a UI compares:
//   `event.target === this.dom`, `parent.children[0] === child`. So the probe
//   asks the same question by four different routes — a stored reference, a
//   children lookup, a querySelector, a parentNode walk — and prints whether
//   they are the SAME OBJECT, not whether they describe the same element.
//
//   REAL ARRAYS. `children` and `querySelectorAll` must be arrays with
//   Array.prototype on them, because what an app does with them is `for…of`,
//   `Array.from`, `.map`, `.filter`. An object with numeric keys and a
//   `length` passes a naive check and is a TypeError at every one of those
//   call sites, so the probe iterates rather than indexing.
//
//   LIFETIME. The registry holds an entry per element and the engine frees
//   nodes on its own schedule. A wrapper for a detached element must keep
//   working (detached is not freed), which is what the reattach section is
//   for: a subtree removed, held, mutated, and put back has to come back with
//   everything it was carrying.
//
// EVERY LINE IS `APP <name>=<value>` and every value is an integer, a boolean
// or a string this file chose. Nothing here prints a float, a clock reading or
// a pointer, and the one geometry line reads a size the appdir's stylesheet
// states in pixels — so the expectation beside this file is written by hand
// from what must be true, not recorded from a run
// (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// ---------------------------------------------------------------------------
// Building a tree
// ---------------------------------------------------------------------------

const panel = document.createElement('div');
panel.id = 'panel';
panel.className = 'ui panel';
document.body.appendChild(panel);

say('panel.nodeType', panel.nodeType);
say('panel.tagName', panel.tagName);
say('panel.id', panel.id);
say('panel.className', panel.className);

// Three children, appended in order, each carrying a text label.
const names = ['alpha', 'beta', 'gamma'];
for (let i = 0; i < names.length; i++) {
    const row = document.createElement('div');
    row.className = 'row';
    row.setAttribute('data-name', names[i]);
    row.textContent = names[i];
    panel.appendChild(row);
}
say('panel.childElementCount', panel.childElementCount);

// ---------------------------------------------------------------------------
// children is a real array
// ---------------------------------------------------------------------------
// Not `children.length === 3` — that is true of a fake. These four lines are
// each a different piece of Array.prototype, and a bare object with numeric
// keys fails every one of them with a TypeError rather than a wrong answer.

const kids = panel.children;
say('children.isArray', Array.isArray(kids));

let joined = '';
for (const kid of kids) { joined = joined + kid.getAttribute('data-name') + ','; }
say('children.forOf', joined);

say('children.map', kids.map(function (k) { return k.textContent; }).join('|'));
say('children.filter', kids.filter(function (k) { return k.textContent !== 'beta'; }).length);

// ---------------------------------------------------------------------------
// Identity, by four routes
// ---------------------------------------------------------------------------

const beta = kids[1];
say('id.childrenStable', panel.children[1] === beta);
say('id.querySelector', document.querySelector('[data-name="beta"]') === beta);
say('id.getElementById', document.getElementById('panel') === panel);
say('id.parentNode', beta.parentNode === panel);
say('id.parentElement', beta.parentElement === panel);
say('id.firstElementChild', panel.firstElementChild === kids[0]);
say('id.lastElementChild', panel.lastElementChild === kids[2]);
say('id.nextSibling', kids[0].nextElementSibling === beta);
say('id.prevSibling', kids[2].previousElementSibling === beta);
say('id.body', document.body === document.querySelector('body'));

// ---------------------------------------------------------------------------
// querySelectorAll, from the document and from an element
// ---------------------------------------------------------------------------
// The element-scoped one must be scoped: `panel.querySelectorAll('.row')` sees
// the three rows and NOT the decoy the page put in the document beside them.

const allRows = document.querySelectorAll('.row');
say('qsa.docIsArray', Array.isArray(allRows));
say('qsa.docCount', allRows.length);
const ownRows = panel.querySelectorAll('.row');
say('qsa.elemCount', ownRows.length);
say('qsa.elemScoped', ownRows.indexOf(beta) === 1);

// ---------------------------------------------------------------------------
// Tree edits
// ---------------------------------------------------------------------------

const delta = document.createElement('div');
delta.className = 'row';
delta.setAttribute('data-name', 'delta');
delta.textContent = 'delta';
panel.insertBefore(delta, beta);
say('edit.afterInsert', panel.children.map(function (k) { return k.textContent; }).join(','));

const epsilon = document.createElement('div');
epsilon.className = 'row';
epsilon.textContent = 'epsilon';
panel.replaceChild(epsilon, delta);
say('edit.afterReplace', panel.children.map(function (k) { return k.textContent; }).join(','));
say('edit.replacedDetached', epsilon.parentNode === panel && delta.parentNode === null);

panel.removeChild(epsilon);
say('edit.afterRemove', panel.children.map(function (k) { return k.textContent; }).join(','));
say('edit.contains.own', panel.contains(beta));
say('edit.contains.gone', panel.contains(epsilon));
say('edit.contains.self', panel.contains(panel));

// `append` with several arguments, and `remove` from the child's own side.
const tail1 = document.createElement('span');
const tail2 = document.createElement('span');
tail1.textContent = 't1';
tail2.textContent = 't2';
panel.append(tail1, tail2);
say('edit.afterAppend', panel.childElementCount);
tail2.remove();
say('edit.afterSelfRemove', panel.childElementCount);
say('edit.selfRemoveDetached', tail2.parentNode === null);

// ---------------------------------------------------------------------------
// Detach, mutate while detached, reattach
// ---------------------------------------------------------------------------
// A held wrapper for a node out of the tree must still be the wrapper the
// registry answers with when the node comes back — this is the case where a
// "free on removal" registry would have thrown its entry away.

panel.removeChild(beta);
say('detach.parent', beta.parentNode === null);
beta.setAttribute('data-name', 'beta2');
beta.textContent = 'beta2';
panel.appendChild(beta);
say('detach.reattachedSame', panel.children[panel.children.length - 1] === beta);
say('detach.keptEdits', beta.getAttribute('data-name') + '/' + beta.textContent);

// ---------------------------------------------------------------------------
// classList
// ---------------------------------------------------------------------------

say('class.initial', panel.className);
panel.classList.add('open');
say('class.afterAdd', panel.classList.contains('open'));
panel.classList.add('open');
say('class.addIsIdempotent', panel.className);
panel.classList.toggle('open');
say('class.afterToggleOff', panel.classList.contains('open'));
panel.classList.toggle('open');
say('class.afterToggleOn', panel.classList.contains('open'));
panel.classList.remove('open');
say('class.afterRemove', panel.className);
say('class.contains.absent', panel.classList.contains('nope'));

// ---------------------------------------------------------------------------
// style
// ---------------------------------------------------------------------------
// Both spellings of every property are registered, so an app written either
// way reads back what it wrote. `setProperty` is the escape hatch for the
// properties not on the curated list, and it must be readable through the same
// object.

panel.style.display = 'block';
panel.style.backgroundColor = 'rgb(1, 2, 3)';
panel.style['margin-top'] = '4px';
panel.style.setProperty('z-index', '7');
say('style.camel', panel.style.display);
say('style.camelReadDashed', panel.style['background-color']);
say('style.dashedReadCamel', panel.style.marginTop);
say('style.setProperty', panel.style.getPropertyValue('z-index'));
say('style.identity', panel.style === panel.style);
panel.style.removeProperty('margin-top');
say('style.afterRemove', panel.style.marginTop === '' || panel.style.marginTop === undefined);

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

panel.setAttribute('title', 'a panel');
say('attr.get', panel.getAttribute('title'));
say('attr.has', panel.hasAttribute('title'));
panel.removeAttribute('title');
say('attr.afterRemove', panel.hasAttribute('title'));
say('attr.missingIsNull', panel.getAttribute('nope') === null);

// ---------------------------------------------------------------------------
// Text and markup
// ---------------------------------------------------------------------------

const scratch = document.createElement('div');
panel.appendChild(scratch);
scratch.innerHTML = '<b class="hit">bold</b><i>ital</i>';
say('html.childCount', scratch.childElementCount);
say('html.firstTag', scratch.firstElementChild.tagName);
say('html.text', scratch.textContent);
say('html.selectorFindsParsed', scratch.querySelectorAll('.hit').length);
scratch.textContent = 'plain';
say('html.textContentClears', scratch.childElementCount);
say('html.textContentSets', scratch.textContent);

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// The appdir's stylesheet gives #panel an explicit 300x120 content box with no
// border or padding, so these are the only numbers they can be — and reading
// them at all is the assertion that the getters flush layout first, because
// this element was appended in the same turn that measures it.

say('geom.offsetWidth', panel.offsetWidth);
say('geom.offsetHeight', panel.offsetHeight);
say('geom.clientWidth', panel.clientWidth);
const rect = panel.getBoundingClientRect();
say('geom.rect', rect.x + ',' + rect.y + ',' + rect.width + ',' + rect.height);
say('geom.rectRightBottom', rect.right + ',' + rect.bottom);

// ---------------------------------------------------------------------------
// Computed style
// ---------------------------------------------------------------------------
// Not the inline style under another name: `width` here is the USED width off
// the layout box, which nothing in `panel.style` knows — the stylesheet set it,
// not the app. `display` is the one the app assigned above, so the two sources
// are both represented and a stub that answered from either alone fails one.

const cs = getComputedStyle(panel);
say('cs.display', cs.display);
say('cs.position', cs.position);
say('cs.width', cs.width);
say('cs.getPropertyValue', cs.getPropertyValue('height'));
// LIVE, not a snapshot: the declaration keeps tracking the element, so a
// property changed after it was handed over reads back changed. `color` and
// not `display`, because #panel is absolutely positioned and CSS blockifies an
// abspos box's display — `inline-block` computes to `block` there, and the
// assertion would pass without the object being live at all.
say('cs.color.before', cs.color);
panel.style.color = 'rgb(10, 20, 30)';
say('cs.color.after', cs.color);
say('cs.writesIgnored', (function () {
    cs.setProperty('color', 'rgb(9, 9, 9)');
    return cs.color;
})());

// ---------------------------------------------------------------------------
// Form controls
// ---------------------------------------------------------------------------
// The three.js editor's whole widget library is this surface.

const input = document.createElement('input');
input.type = 'text';
input.value = 'hello';
panel.appendChild(input);
say('form.input.type', input.type);
say('form.input.value', input.value);
say('form.input.tabIndexDefault', input.tabIndex);
input.tabIndex = 3;
say('form.input.tabIndex', input.tabIndex);
input.placeholder = 'type here';
say('form.input.placeholder', input.placeholder);
say('form.input.disabledDefault', input.disabled);
input.disabled = true;
say('form.input.disabled', input.disabled);
input.disabled = false;
say('form.input.disabledOff', input.disabled);

const check = document.createElement('input');
check.type = 'checkbox';
panel.appendChild(check);
say('form.check.default', check.checked);
check.checked = true;
say('form.check.set', check.checked);
check.checked = false;
say('form.check.clear', check.checked);

// A <select> nobody has laid out yet: the selection lives in `selected`
// attributes rather than in a control, and value/selectedIndex must agree
// about it anyway.
const select = document.createElement('select');
for (const name of ['red', 'green', 'blue']) {
    const option = document.createElement('option');
    option.value = name;
    option.textContent = name;
    select.appendChild(option);
}
panel.appendChild(select);
say('form.select.optionsIsArray', Array.isArray(select.options));
say('form.select.optionCount', select.options.length);
say('form.select.optionIdentity', select.options[1] === select.children[1]);
say('form.select.default', select.value + '/' + select.selectedIndex);
select.value = 'blue';
say('form.select.byValue', select.value + '/' + select.selectedIndex);
select.selectedIndex = 1;
say('form.select.byIndex', select.value + '/' + select.selectedIndex);

const area = document.createElement('textarea');
area.textContent = 'initial';
panel.appendChild(area);
say('form.area.initial', area.value);
area.value = 'edited';
say('form.area.edited', area.value);
say('form.area.textContentUnchanged', area.textContent);

// ---------------------------------------------------------------------------
// Platform globals
// ---------------------------------------------------------------------------

say('plat.btoa', btoa('bro'));
say('plat.atob', atob('dGhyZWU='));
say('plat.base64.roundTrip', atob(btoa('bro/three?=')) === 'bro/three?=');
say('plat.base64.padding', btoa('a') + ',' + btoa('ab') + ',' + btoa('abc'));
say('plat.node.element', Node.ELEMENT_NODE);
say('plat.node.text', Node.TEXT_NODE);
// The interface names: what libraries do with these is test that they RESOLVE
// (see host_platform.cpp). Calling them is not part of the contract.
say('plat.hasEvent', typeof Event !== 'undefined');
say('plat.hasHTMLInputElement', typeof HTMLInputElement !== 'undefined');
say('plat.hasConfirm', typeof confirm === 'function');
say('plat.screenPositive', screen.width > 0 && screen.height > 0);

// ---------------------------------------------------------------------------
// Done, one frame later
// ---------------------------------------------------------------------------
// The tail runs from a requestAnimationFrame rather than here, for one reason:
// queueMicrotask can only be observed after the checkpoint that drains it. The
// ordering asserted is the whole point — the microtask must run AFTER the
// synchronous code that queued it and BEFORE the next frame's callbacks.

let order = '';
queueMicrotask(function () { order = order + 'micro,'; });
order = order + 'sync,';

requestAnimationFrame(function () {
    say('micro.order', order);
    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
});
