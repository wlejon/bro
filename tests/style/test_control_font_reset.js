// Form controls do not inherit the page's text style: they take the UA's
// control font (13.3333px regular Arial; textarea monospace) and reset
// line-height, letter- and word-spacing, text-transform, text-indent and
// text-align, as Chromium's UA sheet does. three.js's css3d examples set
// `font-family: Monospace` on body; their buttons are still Arial there.
document.body.style.cssText = 'font: italic bold 20px/2 Monospace; letter-spacing: 2px; text-transform: uppercase;' +
    'word-spacing: 3px; text-indent: 5px; text-align: right';
document.body.innerHTML = '<button id="b">Hi</button><input id="i"><select id="s"><option>a</option></select>' +
    '<textarea id="t"></textarea>';
const want = { 'font-size': '13.3333px', 'font-weight': '400', 'font-style': 'normal', 'line-height': 'normal',
               'letter-spacing': 'normal', 'word-spacing': '0px', 'text-transform': 'none', 'text-indent': '0px' };
for (const id of ['b', 'i', 's', 't']) {
    const cs = getComputedStyle(document.getElementById(id));
    for (const [p, v] of Object.entries(want)) assert(cs.getPropertyValue(p) === v, `${id} ${p}: ${cs.getPropertyValue(p)}`);
    assert(cs.fontFamily === (id === 't' ? 'monospace' : 'Arial'), `${id} font-family: ${cs.fontFamily}`);
}
assert(getComputedStyle(document.getElementById('b')).textAlign === 'center', 'a button centres');
assert(getComputedStyle(document.getElementById('i')).textAlign === 'start', 'an input starts');
console.log('test_control_font_reset: OK');
