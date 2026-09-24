// The CSS namespace (supports / escape), the Option legacy factory, and
// HTMLDetailsElement.open reflecting its attribute. Plus: a selector list
// splits only at top-level commas, so a comma inside a quoted attribute value
// stays part of its selector.

// --- CSS ---------------------------------------------------------------------
assert(typeof CSS === 'object', 'CSS is a namespace object, got ' + typeof CSS);
assert(CSS.supports('display', 'grid') === true, 'CSS.supports(display, grid)');
assert(CSS.supports('display', 'nonsense') === false, 'CSS.supports rejects a bad display');
assert(CSS.supports('color', 'rgb(1, 2, 3)') === true, 'CSS.supports accepts an rgb() colour');
assert(CSS.supports('not-a-property', '1px') === false, 'CSS.supports rejects an unknown property');
assert(CSS.supports('(display: flex) and (color: red)') === true, 'condition form');
assert(CSS.supports('display: flex') === true, 'bare declaration form');
assert(CSS.supports('not (display: flex)') === false, 'not () form');
assert(CSS.escape('a b') === 'a\\ b', 'CSS.escape escapes a space, got ' + CSS.escape('a b'));
assert(CSS.escape('1x') === '\\31 x', 'CSS.escape escapes a leading digit, got ' + CSS.escape('1x'));
assert(CSS.escape('-') === '\\-', 'CSS.escape escapes a lone hyphen');
assert(CSS.escape('a.b#c') === 'a\\.b\\#c', 'CSS.escape escapes selector syntax, got ' + CSS.escape('a.b#c'));
assert(CSS.escape('ok_name-1') === 'ok_name-1', 'CSS.escape leaves an identifier alone');

// --- Option ------------------------------------------------------------------
assert(typeof Option === 'function', 'Option is a constructor');
const o = new Option('Label', 'val', true, false);
assert(o instanceof HTMLOptionElement, 'new Option() is an HTMLOptionElement');
assert(o.tagName === 'OPTION', 'tagName OPTION, got ' + o.tagName);
assert(o.textContent === 'Label', 'text, got ' + o.textContent);
assert(o.getAttribute('value') === 'val', 'value attribute, got ' + o.getAttribute('value'));
assert(o.selected === true, 'defaultSelected selects');
const sel = document.createElement('select');
sel.appendChild(new Option('A', 'a'));
sel.appendChild(new Option('B', 'b', false, true));
document.getElementById('root').appendChild(sel);
flush();
assert(sel.value === 'b', 'the selected Option is the select value, got ' + sel.value);
assert(new Option().textContent === '', 'new Option() is empty');

// --- details.open ------------------------------------------------------------
const d = document.createElement('details');
d.innerHTML = '<summary>s</summary><div id="body" style="height:40px">x</div>';
document.getElementById('root').appendChild(d);
flush();
assert(d.open === false, 'closed details reads open false');
assert(d instanceof HTMLDetailsElement, 'details is an HTMLDetailsElement');
d.open = true;
assert(d.hasAttribute('open'), 'open = true sets the attribute');
flush();
assert(document.getElementById('body').getBoundingClientRect().height > 0, 'open details shows its body');
d.open = false;
assert(!d.hasAttribute('open'), 'open = false removes the attribute');
d.setAttribute('open', '');
assert(d.open === true, 'the attribute reads back as open');

// --- selector list commas ----------------------------------------------------
const holder = document.createElement('div');
holder.innerHTML = '<i data-x="1,2" id="hit"></i><i data-x="1" id="miss"></i><b id="b"></b>';
document.getElementById('root').appendChild(holder);
assert(document.querySelector('[data-x="1,2"]') === document.getElementById('hit'),
    'querySelector keeps a quoted comma inside its attribute selector');
assert(document.querySelectorAll('[data-x="1,2"]').length === 1,
    'querySelectorAll matches only the one element, got ' + document.querySelectorAll('[data-x="1,2"]').length);
assert(!document.getElementById('miss').matches('[data-x="1,2"]'), 'matches() is false for the other one');
assert(document.querySelectorAll('[data-x="1,2"], b').length === 2, 'a real top-level comma still splits');
assert(document.querySelectorAll(':is([data-x="1,2"], b)').length === 2, 'quoted comma inside :is()');
