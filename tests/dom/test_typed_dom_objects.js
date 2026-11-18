// The DOM's helper objects are instances of their interfaces, as in a
// browser: element.style and getComputedStyle() are CSSStyleDeclarations,
// classList / relList DOMTokenLists (indexed, validated, ordered sets),
// dataset a DOMStringMap, localStorage a Storage with its members on
// Storage.prototype, matchMedia() a MediaQueryList.
const T = (v) => Object.prototype.toString.call(v).slice(8, -1);
const eq = (got, want, what) => assert(got === want, `${what}: got ${got}, want ${want}`);
document.body.innerHTML = '<div id="d" class="a b a" style="color:red" data-user-id="7"></div><a id="l" rel="noopener next"></a>';
const d = document.getElementById('d');
eq(`${T(d.style)} ${d.style instanceof CSSStyleDeclaration} ${d.style.constructor === CSSStyleDeclaration} ${d.style.color}`,
   'CSSStyleDeclaration true true red', 'element.style');
eq(`${[...d.style].join()} ${d.style.parentRule}`, 'color null', 'style iterates its properties');
const cs = getComputedStyle(d);
eq(`${T(cs)} ${cs instanceof CSSStyleDeclaration} ${cs.color}`, 'CSSStyleDeclaration true rgb(255, 0, 0)', 'computed style');

const cl = d.classList;
eq(`${T(cl)} ${cl instanceof DOMTokenList} ${cl.constructor === DOMTokenList}`, 'DOMTokenList true true', 'classList');
eq(`${cl.length} ${cl[0]} ${cl[1]} ${cl[2]} ${cl.item(1)} ${[...cl].join()} ${Object.keys(cl).join()}`,
   '2 a b undefined b a,b 0,1', 'a duplicate-free indexed set');
eq(String(cl), 'a b a', 'toString is the attribute');
let err = null;
try { cl.add(''); } catch (e) { err = e.name; }
eq(err, 'SyntaxError', 'an empty token');
err = null;
try { cl.toggle('x y'); } catch (e) { err = e.name; }
eq(err, 'InvalidCharacterError', 'a token with a space');
cl.replace('a', 'b');
eq(d.className, 'b', 'replace into a present token drops the old');
const plain = document.createElement('p');
plain.classList.remove('z');
plain.classList.add();
eq(plain.hasAttribute('class'), false, 'an empty update does not create the attribute');
eq(typeof DOMTokenList.prototype, 'object', 'DOMTokenList.prototype');
err = null;
try { new DOMTokenList(); } catch (e) { err = e.name; }
eq(err, 'TypeError', 'DOMTokenList is not constructible');

const rl = document.getElementById('l').relList;
eq(`${T(rl)} ${rl.length} ${rl.contains('next')}`, 'DOMTokenList 2 true', 'a.relList');
rl.remove('next');
eq(document.getElementById('l').rel, 'noopener', 'relList writes rel');
eq(T(document.createElement('link').relList), 'DOMTokenList', 'link.relList');

eq(`${T(d.dataset)} ${d.dataset instanceof DOMStringMap} ${d.dataset.userId}`, 'DOMStringMap true 7', 'dataset');

eq(`${T(localStorage)} ${localStorage instanceof Storage} ${T(sessionStorage)} ${typeof Storage.prototype.getItem}`,
   'Storage true Storage function', 'Storage');
localStorage.clear();
localStorage.setItem('k', 'v');
sessionStorage.s = '1';
eq(`${Storage.prototype.getItem.call(localStorage, 'k')} ${localStorage.length} ${Object.keys(localStorage).join()} ${sessionStorage.getItem('s')}`,
   'v 1 k 1', 'Storage members on the prototype, items as properties');
err = null;
try { Storage.prototype.getItem.call({}, 'k'); } catch (e) { err = e.name; }
eq(err, 'TypeError', 'a Storage member on another object');
localStorage.clear();
sessionStorage.clear();

const mq = matchMedia('(min-width: 1px)');
eq(`${T(mq)} ${mq instanceof MediaQueryList} ${mq.matches}`, 'MediaQueryList true true', 'matchMedia');
console.log('test_typed_dom_objects: OK');
