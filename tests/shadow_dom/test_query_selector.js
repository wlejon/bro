// querySelector / querySelectorAll on a shadow root and on a DocumentFragment
// run the full selector engine (combinators, attributes, pseudo-classes), and
// their combinators stay inside that tree.

const root = document.getElementById('root');
const host = document.createElement('div');
host.className = 'outer';
root.appendChild(host);
const sr = host.attachShadow({ mode: 'open' });
sr.innerHTML =
    '<section class="a"><div class="row"><span id="s1">one</span></div>' +
    '<p><span id="s2" data-k="v">two</span></p></section>' +
    '<span id="s3">three</span>';

const ids = (list) => Array.from(list).map((e) => e.id).join(',');

assert(sr.querySelector('div > span') && sr.querySelector('div > span').id === 's1',
    'child combinator in a shadow root, got ' + (sr.querySelector('div > span') || {}).id);
assert(ids(sr.querySelectorAll('section span')) === 's1,s2', 'descendant combinator, got ' + ids(sr.querySelectorAll('section span')));
assert(ids(sr.querySelectorAll('span[data-k="v"]')) === 's2', 'attribute selector');
assert(ids(sr.querySelectorAll('span:first-child')) === 's1,s2', ':first-child, got ' + ids(sr.querySelectorAll('span:first-child')));
assert(ids(sr.querySelectorAll('p span, .row span')) === 's1,s2', 'selector list in tree order');
assert(ids(sr.querySelectorAll('span')) === 's1,s2,s3', 'every span in tree order');

// The host is a <div class="outer">; a shadow-tree query must not match through it.
assert(sr.querySelector('div > #s3') === null, 'the child combinator does not reach the shadow host');
assert(sr.querySelector('.outer span') === null, 'the descendant combinator does not reach the shadow host');
assert(sr.querySelector('#s1').matches('.outer span') === false,
    'matches() inside a shadow tree does not walk out to the host');
assert(sr.querySelector('#s1').closest('.outer') === null, 'closest() stops at the shadow root');
// ...but :host does name the host.
assert(ids(sr.querySelectorAll(':host > span')) === 's3', ':host > span, got ' + ids(sr.querySelectorAll(':host > span')));

// Light-DOM queries don't see into the shadow tree.
assert(host.querySelector('span') === null, 'light DOM query does not enter the shadow tree');

// DocumentFragment
const frag = document.createDocumentFragment();
const ul = document.createElement('ul');
for (let i = 0; i < 3; i++) {
    const li = document.createElement('li');
    li.id = 'li' + i;
    if (i === 1) li.className = 'mid';
    ul.appendChild(li);
}
frag.appendChild(ul);
const loose = document.createElement('li');
loose.id = 'loose';
frag.appendChild(loose);
assert(ids(frag.querySelectorAll('ul > li')) === 'li0,li1,li2', 'fragment child combinator, got ' + ids(frag.querySelectorAll('ul > li')));
assert(ids(frag.querySelectorAll('li:not(.mid)')) === 'li0,li2,loose', 'fragment :not()');
assert(frag.querySelector('li:last-child').id === 'li2', 'fragment querySelector returns the first in tree order');
assert(frag.querySelector('ul li + li').id === 'li1', 'fragment sibling combinator');
assert(frag.querySelector('table') === null, 'no match is null');

// Template content is a fragment too.
const tpl = document.createElement('template');
tpl.innerHTML = '<div class="card"><h2>t</h2><p class="body">b</p></div>';
assert(tpl.content.querySelector('.card > .body') !== null, 'template content query');

console.log('test_query_selector: OK');
