// Test querySelector and querySelectorAll

const root = document.getElementById('root');

// Build a small tree
root.innerHTML = '<div class="a"><span class="b">hello</span><span class="c">world</span></div><div class="d"></div>';
flush();

// querySelector by class
const a = root.querySelector('.a');
assert(a !== null, 'querySelector .a found');
assert(a.className === 'a' || a.getAttribute('class') === 'a', 'querySelector .a has correct class');

// querySelector by tag
const span = root.querySelector('span');
assert(span !== null, 'querySelector span found');
assert(span.tagName === 'SPAN', 'first span found');

// querySelectorAll
const spans = root.querySelectorAll('span');
assert(spans.length === 2, 'querySelectorAll finds 2 spans');

// querySelectorAll by class
const ds = root.querySelectorAll('.d');
assert(ds.length === 1, 'querySelectorAll .d finds 1');

// getElementById
root.innerHTML = '<div id="target">found</div>';
flush();
const target = document.getElementById('target');
assert(target !== null, 'getElementById finds element');
assert(target.textContent === 'found', 'getElementById returns correct element');

// querySelector with compound selector
root.innerHTML = '<div class="x"><p class="y">inner</p></div>';
flush();
const inner = root.querySelector('.x .y');
assert(inner !== null, 'compound selector works');
assert(inner.textContent === 'inner', 'compound selector finds correct element');

// querySelector returns null for no match
const none = root.querySelector('.nonexistent');
assert(none === null, 'querySelector returns null for no match');

// Escaped CSS identifiers match normally
root.innerHTML = '<div class="w-1/2" id="a:b">escaped</div>';
flush();
const escCls = document.querySelector('.w-1\\/2');
assert(escCls !== null && escCls.textContent === 'escaped', '.w-1\\/2 matches w-1/2');
const escId = document.querySelector('#a\\:b');
assert(escId !== null && escId.textContent === 'escaped', '#a\\:b matches a:b');

// Invalid selectors throw SyntaxError DOMException and never match <html>
const frag = document.createDocumentFragment();
frag.appendChild(document.createElement('div'));
const host = document.createElement('div');
const sr = host.attachShadow({ mode: 'open' });
sr.innerHTML = '<span>shadow</span>';

function assertSyntaxError(fn, label) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught !== null, label + ' throws');
    assert(caught instanceof Error, label + ' is instanceof Error');
    if (typeof DOMException === 'function') {
        assert(caught instanceof DOMException, label + ' is instanceof DOMException');
        assert(caught.code === DOMException.SYNTAX_ERR, label + ' code === SYNTAX_ERR (got ' + caught.code + ')');
    }
    assert(caught.name === 'SyntaxError', label + ' name === SyntaxError (got ' + caught.name + ')');
}

for (const bad of ['0.13-25', '', '   ', 'div >', '> div', '[unclosed', '.123', 'div, ']) {
    assertSyntaxError(() => document.querySelector(bad), 'document.querySelector(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => document.querySelectorAll(bad), 'document.querySelectorAll(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => root.querySelector(bad), 'el.querySelector(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => root.querySelectorAll(bad), 'el.querySelectorAll(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => root.matches(bad), 'el.matches(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => root.closest(bad), 'el.closest(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => frag.querySelector(bad), 'frag.querySelector(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => frag.querySelectorAll(bad), 'frag.querySelectorAll(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => sr.querySelector(bad), 'shadow.querySelector(' + JSON.stringify(bad) + ')');
    assertSyntaxError(() => sr.querySelectorAll(bad), 'shadow.querySelectorAll(' + JSON.stringify(bad) + ')');
}

// Cleanup
root.innerHTML = '';
