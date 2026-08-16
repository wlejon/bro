// `text-transform` is a rendering property that inherits, so a
// `<select style="text-transform:uppercase">` paints its option labels
// uppercased — both the closed control and the open dropdown. bro draws the
// select itself (src/layout/el_select.cpp), so the transform has to be applied
// where that label is built rather than falling out of the text pipeline.
//
// The three.js editor styles every select this way; without it the sidebar
// reads "camera / solid / default" where every browser shows "CAMERA / SOLID /
// DEFAULT". What must NOT change is the value the page reads back — that stays
// the author's string.

const root = document.getElementById('root');
root.innerHTML =
    '<select id="plain" style="font:16px Arial">' +
    '<option value="camera">camera</option>' +
    '<option value="solid">solid</option>' +
    '</select>' +
    '<select id="upper" style="font:16px Arial;text-transform:uppercase">' +
    '<option value="camera">camera</option>' +
    '<option value="solid">solid</option>' +
    '</select>' +
    '<select id="cap" style="font:16px Arial;text-transform:capitalize">' +
    '<option value="camera">camera</option>' +
    '</select>';
flush();

const plain = document.getElementById('plain');
const upper = document.getElementById('upper');
const cap = document.getElementById('cap');

// The transform inherits into the <option>, which is where el_select reads it.
assert(getComputedStyle(upper.querySelector('option')).textTransform === 'uppercase',
       'text-transform inherits from the select into its options, got: ' +
       getComputedStyle(upper.querySelector('option')).textTransform);

// The intrinsic width of a select is its widest option label. Uppercasing
// makes that label wider in any proportional font, so the control grows —
// which is the observable proof the painted string went through the transform
// rather than the raw text.
const wPlain = plain.getBoundingClientRect().width;
const wUpper = upper.getBoundingClientRect().width;
assert(wUpper > wPlain,
       'the uppercased control is wider than the untransformed one (' +
       wUpper + ' vs ' + wPlain + ')');

// capitalize touches only the first letter, so it lands between the two.
const wCap = cap.getBoundingClientRect().width;
assert(wCap > wPlain && wCap < wUpper,
       'capitalize widens less than uppercase (' + wCap + ' vs plain ' +
       wPlain + ', upper ' + wUpper + ')');

// The transform is presentational. Everything the page reads back is the
// author's string, untouched.
assert(upper.value === 'camera', 'select.value is the authored value, got: ' + upper.value);
assert(upper.options[0].textContent === 'camera',
       'option.textContent is the authored text, got: ' + upper.options[0].textContent);
upper.selectedIndex = 1;
assert(upper.value === 'solid',
       'and it stays authored after a selection change, got: ' + upper.value);

// An explicit `text-transform: none` on the option wins over the inherited
// uppercase, so the control measures like the plain one again.
upper.querySelectorAll('option').forEach(o => { o.style.textTransform = 'none'; });
flush();
assert(Math.abs(upper.getBoundingClientRect().width - wPlain) < 0.5,
       'text-transform:none on the option overrides the inherited uppercase (' +
       upper.getBoundingClientRect().width + ' vs ' + wPlain + ')');

root.innerHTML = '';
console.log('PASS: <select> option labels honour text-transform, values do not');
