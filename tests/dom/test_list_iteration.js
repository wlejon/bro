// The DOM's lists are iterable (host_list_iteration.h): for…of, spread and
// destructuring over a live HTMLCollection (getElementsByTagName /
// ClassName / Name, also an XML document's), NodeList, DOMTokenList (with
// forEach / entries / keys / values) and the rest.

document.body.innerHTML = '<p class="a b">x</p><p class="a">y</p>';
const ps = document.getElementsByTagName('p');
assert(typeof ps[Symbol.iterator] === 'function', 'HTMLCollection has [Symbol.iterator]');
const [first] = ps;
assert(first && first.textContent === 'x', 'destructuring an HTMLCollection');
assert([...document.getElementsByClassName('a')].length === 2, 'spread of getElementsByClassName');
let n = 0;
for (const p of ps) { n++; assert(p.tagName === 'P', 'for…of yields the elements'); }
assert(n === 2, 'for…of over the collection, got ' + n);
// Live: the iterator walks what the collection holds as it goes.
document.body.insertAdjacentHTML('beforeend', '<p>z</p>');
assert(Array.from(ps).length === 3, 'Array.from sees the live collection, got ' + Array.from(ps).length);
assert(ps.forEach === undefined, 'HTMLCollection has no forEach, as on the web');

const xml = new DOMParser().parseFromString('<r><d k="1"/></r>', 'text/xml');
const [d] = xml.getElementsByTagName('d');
assert(d && d.getAttribute('k') === '1', 'an XML document\'s collection iterates');

const cl = first.classList;
assert(JSON.stringify([...cl]) === '["a","b"]', 'spread of classList, got ' + JSON.stringify([...cl]));
const seen = [];
cl.forEach((c, i) => seen.push(i + ':' + c));
assert(seen.join(',') === '0:a,1:b', 'classList.forEach, got ' + seen.join(','));
assert(JSON.stringify([...cl.entries()]) === '[[0,"a"],[1,"b"]]', 'classList.entries');
assert(JSON.stringify([...cl.keys()]) === '[0,1]' && JSON.stringify([...cl.values()]) === '["a","b"]',
       'classList.keys / values');

for (const [what, list] of [['querySelectorAll', document.querySelectorAll('p')], ['childNodes', document.body.childNodes],
                            ['children', document.body.children], ['attributes', first.attributes]]) {
    assert(typeof list[Symbol.iterator] === 'function' && typeof list.forEach === 'function', what + ' is iterable');
}

console.log('test_list_iteration: OK');
