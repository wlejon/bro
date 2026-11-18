// A family that is not installed is tried under its alternate name, as Blink
// and WebKit do: Courier / Courier New, Times / Times New Roman, Helvetica /
// Arial. On Linux (fontconfig, no Microsoft or Adobe fonts) each pair lands on
// one metric-compatible face, Liberation Mono / Serif / Sans, as in Chromium;
// Phaser's default text font is 'Courier'.
const span = (family) => {
    const s = document.createElement('span');
    s.style.font = `16px ${family}`;
    s.textContent = 'abcdefghijMW iiii';
    document.body.appendChild(s);
    return s.getBoundingClientRect().width;
};
const ctx = document.createElement('canvas').getContext('2d');
const measure = (family) => { ctx.font = `16px ${family}`; return ctx.measureText('abcdefghijMW iiii').width; };
if (navigator.platform.startsWith('Linux')) {
    for (const [a, b] of [['Courier', '"Courier New"'], ['Times', '"Times New Roman"'], ['Helvetica', 'Arial']]) {
        assert(span(a) === span(b), `${a} lays out as ${b}: ${span(a)} vs ${span(b)}`);
        assert(measure(a) === measure(b), `${a} measures as ${b}: ${measure(a)} vs ${measure(b)}`);
    }
    assert(span('Courier') === span('monospace'), 'Courier is the monospace face');
    assert(span('Courier') !== span('Menlo'), 'an unknown family is not monospace');
}
console.log('test_font_alternate_names: OK');
