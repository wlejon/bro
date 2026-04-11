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

// Cleanup
root.innerHTML = '';
