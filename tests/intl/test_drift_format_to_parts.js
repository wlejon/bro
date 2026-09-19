// `formatToParts` on the three Intl formatters that lost it in the port.
//
// Intl.NumberFormat kept it; DateTimeFormat, ListFormat and RelativeTimeFormat
// did not, so a caller that maps over the parts — the normal way to style the
// currency symbol differently from the digits, or to wrap the unit in a
// <span> — threw "not a function" instead. These three formatters do not
// track where their pieces came from, so, as in the old stack, they report the
// formatted string as a single `literal` part: the invariant a caller relies
// on is that joining the values reproduces `format()`, and that holds.

const d = new Date(2020, 0, 2, 13, 45, 30);

// --- Intl.DateTimeFormat --------------------------------------------------
const dtf = new Intl.DateTimeFormat('en-US', {
    year: 'numeric', month: 'long', day: 'numeric',
});
assert(typeof dtf.formatToParts === 'function',
       'Intl.DateTimeFormat#formatToParts exists');
const dParts = dtf.formatToParts(d);
assert(Array.isArray(dParts), 'it returns an array');
assert(dParts.length >= 1, 'with at least one part');
assert(typeof dParts[0].type === 'string', 'each part has a type');
assert(typeof dParts[0].value === 'string', 'each part has a value');
assert(dParts.map(function (p) { return p.value; }).join('') === dtf.format(d),
       'joining the part values reproduces format()');

// --- Intl.ListFormat ------------------------------------------------------
const lf = new Intl.ListFormat('en', { type: 'conjunction' });
assert(typeof lf.formatToParts === 'function',
       'Intl.ListFormat#formatToParts exists');
const lParts = lf.formatToParts(['a', 'b', 'c']);
assert(Array.isArray(lParts), 'ListFormat parts is an array');
assert(lParts.map(function (p) { return p.value; }).join('') === lf.format(['a', 'b', 'c']),
       'joining the ListFormat parts reproduces format()');

// --- Intl.RelativeTimeFormat ---------------------------------------------
const rtf = new Intl.RelativeTimeFormat('en', { numeric: 'always' });
assert(typeof rtf.formatToParts === 'function',
       'Intl.RelativeTimeFormat#formatToParts exists');
const rParts = rtf.formatToParts(-3, 'day');
assert(Array.isArray(rParts), 'RelativeTimeFormat parts is an array');
assert(rParts.map(function (p) { return p.value; }).join('') === rtf.format(-3, 'day'),
       'joining the RelativeTimeFormat parts reproduces format()');

// --- Intl.NumberFormat still reports real typed parts ---------------------
const nf = new Intl.NumberFormat('en-US');
const nParts = nf.formatToParts(1234.5);
assert(nParts.length > 1, 'NumberFormat still breaks a number into real parts');
assert(nParts.map(function (p) { return p.value; }).join('') === nf.format(1234.5),
       'and they still join back to format()');
