// setAttribute's value is a WebIDL DOMString: ToString of whatever is passed,
// as Chromium has it. An array is its join (d3's
// `.attr('viewBox', [-w / 2, -h / 2, w, h])` set an empty viewBox, so its force
// graph drew in the corner), an object its toString(), null "null",
// undefined "undefined"; a Symbol throws TypeError.

const d = document.createElement('div');
d.setAttribute('a', [1, 2, 3]);
assert(d.getAttribute('a') === '1,2,3', 'an array is joined, got ' + d.getAttribute('a'));
d.setAttribute('b', { toString() { return 'obj'; } });
assert(d.getAttribute('b') === 'obj', 'an object is its toString(), got ' + d.getAttribute('b'));
d.setAttribute('c', null);
assert(d.getAttribute('c') === 'null', 'null is "null", got ' + d.getAttribute('c'));
d.setAttribute('u', undefined);
assert(d.getAttribute('u') === 'undefined', 'undefined is "undefined", got ' + d.getAttribute('u'));
d.setAttribute('n', 1.5);
assert(d.getAttribute('n') === '1.5', 'a number is its ToString');
let threw = null;
try { d.setAttribute('s', Symbol('x')); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'a Symbol is a TypeError');
let thrown = null;
try { d.setAttribute('t', { toString() { throw new RangeError('mine'); } }); } catch (e) { thrown = e; }
assert(thrown instanceof RangeError && !d.hasAttribute('t'), 'a throwing toString propagates and sets nothing');

const ns = 'http://www.w3.org/2000/svg';
d.setAttributeNS(null, 'ns', [4, 5]);
assert(d.getAttribute('ns') === '4,5', 'setAttributeNS converts the same way');
const canvas = document.createElement('canvas');
canvas.setAttribute('width', { valueOf() { return 7; }, toString() { return '64'; } });
assert(canvas.width === 64, 'a canvas width set through toString, got ' + canvas.width);

// The case that found it: an SVG viewBox from an array maps user space.
document.body.innerHTML = '<div id="host" style="margin:0"></div>';
document.body.style.margin = '0';
const svg = document.createElementNS(ns, 'svg');
svg.setAttribute('width', 200);
svg.setAttribute('height', 100);
svg.setAttribute('viewBox', [-100, -50, 200, 100]);
svg.style.display = 'block';
const c = document.createElementNS(ns, 'circle');
c.setAttribute('r', 5);
svg.appendChild(c);
document.getElementById('host').appendChild(svg);
flush();
const r = c.getBoundingClientRect();
assert(r.x === 95 && r.y === 45, 'the circle at the origin is in the middle of the svg, got ' + r.x + ',' + r.y);

console.log('test_set_attribute_tostring: OK');
