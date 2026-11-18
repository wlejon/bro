// Every supported CSS property is a member of every CSSStyleDeclaration, set
// or not (`'transform' in el.style`), as on the web: feature detection asks
// exactly this. Leaflet's testProp(['transform', 'webkitTransform', ...])
// got false for all of them and wrote every position to style['false'], so
// its SVG overlay and markers never moved. -webkit- aliases are members;
// Moz/ms/O prefixes and unknown names are not; a custom property only once set.
const s = document.createElement('div').style;
for (const k of ['transform', 'color', 'gridAutoFlow', 'background-color', 'cssFloat', 'webkitTransform', 'WebkitTransform', 'userSelect', 'margin'])
    assert(k in s, k + ' is a member');
for (const k of ['MozTransform', 'msTransform', 'OTransform', 'notAProperty', '--x'])
    assert(!(k in s), k + ' is not a member');
s.setProperty('--x', '1');
assert('--x' in s, 'a set custom property is');
assert(Object.keys(s).length <= 2, 'enumeration lists the set properties only: ' + Object.keys(s).join());
assert('transform' in getComputedStyle(document.body), 'computed style too');
console.log('test_style_membership: OK');
