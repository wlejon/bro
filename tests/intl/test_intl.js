// ECMA-402 (`Intl`) is absent from QuickJS, so bro installs a polyfill into
// every realm at context creation (src/js/js/intl_polyfill.js). Real pages
// reach for it constantly and mostly without naming it: the three.js editor's
// status bar says "1 Object" / "2 Objects" through Intl.PluralRules, and
// (1234567).toLocaleString() is how any UI writes a number a human can read.
//
// The tables behind this are deliberately small — the point is that the common
// shapes answer correctly rather than throwing ReferenceError. These assertions
// pin the behaviour a page can actually rely on.

assert(typeof Intl === 'object' && Intl !== null, 'Intl exists');
assert(Object.prototype.toString.call(Intl) === '[object Intl]',
       'Intl has the spec toStringTag');

// --- NumberFormat ---------------------------------------------------------
assert(new Intl.NumberFormat('en-US').format(1234567) === '1,234,567',
       'en-US groups with commas: ' + new Intl.NumberFormat('en-US').format(1234567));
assert(new Intl.NumberFormat('de-DE').format(1234567.5) === '1.234.567,5',
       'de-DE swaps the group and decimal separators: ' +
       new Intl.NumberFormat('de-DE').format(1234567.5));
assert(new Intl.NumberFormat('en-IN').format(1234567) === '12,34,567',
       'en-IN uses lakh/crore grouping: ' +
       new Intl.NumberFormat('en-IN').format(1234567));
assert(new Intl.NumberFormat('en-US', { useGrouping: false }).format(1234567) === '1234567',
       'useGrouping:false turns grouping off');

const f2 = new Intl.NumberFormat('en-US',
    { minimumFractionDigits: 2, maximumFractionDigits: 2 });
assert(f2.format(3.14159) === '3.14', 'fraction digits round: ' + f2.format(3.14159));
assert(f2.format(2) === '2.00', 'and pad: ' + f2.format(2));
assert(new Intl.NumberFormat('en-US', { style: 'percent' }).format(0.256) === '26%',
       'percent scales by 100: ' +
       new Intl.NumberFormat('en-US', { style: 'percent' }).format(0.256));
assert(new Intl.NumberFormat('en-US',
    { style: 'currency', currency: 'USD' }).format(9.5) === '$9.50',
       'currency picks the symbol and its default 2 digits: ' +
       new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' }).format(9.5));
assert(new Intl.NumberFormat('en-US',
    { style: 'currency', currency: 'JPY' }).format(1200) === '¥1,200',
       'JPY has no minor unit');
assert(new Intl.NumberFormat('en-US').format(-42) === '-42', 'negatives keep the sign');

const parts = new Intl.NumberFormat('en-US').formatToParts(1234.5);
assert(Array.isArray(parts) && parts.length > 1, 'formatToParts returns a part list');
assert(parts.map(p => p.value).join('') === '1,234.5',
       'the parts concatenate back to the formatted string');
assert(parts.some(p => p.type === 'group') && parts.some(p => p.type === 'decimal'),
       'and are typed (group, decimal)');

// Called without `new`, as a plain function — the spec allows it.
assert(Intl.NumberFormat('en-US').format(1000) === '1,000',
       'NumberFormat works without new');
assert(new Intl.NumberFormat('en-US').resolvedOptions().locale === 'en-US',
       'resolvedOptions reports the locale');

// --- PluralRules ----------------------------------------------------------
// This is the one the editor's "N Object(s)" depends on.
const en = new Intl.PluralRules('en-US');
assert(en.select(0) === 'other', 'en: 0 is other');
assert(en.select(1) === 'one', 'en: 1 is one');
assert(en.select(2) === 'other', 'en: 2 is other');
assert(new Intl.PluralRules('ja').select(1) === 'other', 'ja has no singular');
assert(new Intl.PluralRules('fr').select(0) === 'one', 'fr counts 0 as one');
const ru = new Intl.PluralRules('ru');
assert(ru.select(1) === 'one' && ru.select(2) === 'few' && ru.select(5) === 'many',
       'ru has one/few/many');
assert(new Intl.PluralRules('en', { type: 'ordinal' }).select(2) === 'two',
       'en ordinals: 2nd is "two"');
assert(new Intl.PluralRules('en', { type: 'ordinal' }).select(3) === 'few',
       'en ordinals: 3rd is "few"');
assert(new Intl.PluralRules('en', { type: 'ordinal' }).select(11) === 'other',
       'en ordinals: 11th is "other"');

// --- Collator -------------------------------------------------------------
const coll = new Intl.Collator('en');
assert(coll.compare('a', 'b') < 0 && coll.compare('b', 'a') > 0 &&
       coll.compare('a', 'a') === 0, 'Collator orders');
// `.compare` is bound, so it can be handed straight to Array#sort.
const names = ['item10', 'item2', 'item1'];
assert(names.slice().sort(new Intl.Collator('en', { numeric: true }).compare)
           .join(',') === 'item1,item2,item10',
       'numeric collation sorts item2 before item10');
assert(new Intl.Collator('en', { sensitivity: 'base' }).compare('Résumé', 'resume') === 0,
       'sensitivity:base ignores case and accents');
assert('a'.localeCompare('b') < 0, 'String#localeCompare routes through Collator');

// --- Number/Date prototype hooks -----------------------------------------
assert((1234567).toLocaleString('en-US') === '1,234,567',
       'Number#toLocaleString groups (QuickJS aliases it to toString without this)');
const d = new Date(2024, 2, 5, 14, 30, 0);   // 5 March 2024, 14:30 local
assert(d.toLocaleDateString('en-US') === '3/5/2024',
       'en-US writes the month first: ' + d.toLocaleDateString('en-US'));
assert(d.toLocaleDateString('en-GB') === '5/3/2024',
       'en-GB writes the day first: ' + d.toLocaleDateString('en-GB'));
assert(d.toLocaleTimeString('en-US').indexOf('2:30') === 0,
       'en-US uses a 12-hour clock: ' + d.toLocaleTimeString('en-US'));
assert(d.toLocaleTimeString('de-DE').indexOf('14:30') === 0,
       'de-DE uses 24: ' + d.toLocaleTimeString('de-DE'));
assert(new Intl.DateTimeFormat('en-US', { month: 'long', day: 'numeric',
                                          year: 'numeric' }).format(d) === 'March 5, 2024',
       'month:long spells the month out');

// --- ListFormat / RelativeTimeFormat / DisplayNames -----------------------
assert(new Intl.ListFormat('en').format(['a', 'b', 'c']) === 'a, b, and c',
       'ListFormat joins with the Oxford comma');
assert(new Intl.ListFormat('en', { type: 'disjunction' }).format(['a', 'b']) === 'a or b',
       'disjunction says or');
const rtf = new Intl.RelativeTimeFormat('en');
assert(rtf.format(-1, 'day') === '1 day ago', 'RelativeTimeFormat: past');
assert(rtf.format(3, 'days') === 'in 3 days', 'RelativeTimeFormat: future, pluralized');
assert(new Intl.RelativeTimeFormat('en', { numeric: 'auto' }).format(-1, 'day')
       === 'yesterday', 'numeric:auto gets the word');
assert(new Intl.DisplayNames(['en'], { type: 'region' }).of('FR') === 'France',
       'DisplayNames names a region');
assert(new Intl.DisplayNames(['en'], { type: 'language' }).of('fr') === 'French',
       'and a language, in the display locale');
assert(new Intl.DisplayNames(['fr'], { type: 'language' }).of('fr') === 'français',
       'asking a locale for its own name gets the endonym');
assert(new Intl.DisplayNames(['en'], { type: 'region' }).of('ZZ') === 'ZZ',
       'an unknown code falls back to the code itself');
let threw = false;
try { new Intl.DisplayNames(['en'], {}); } catch (e) { threw = true; }
assert(threw, 'DisplayNames requires a type');

// --- getCanonicalLocales --------------------------------------------------
assert(Intl.getCanonicalLocales('en-us').join(',') === 'en-US',
       'getCanonicalLocales fixes the case of the region subtag');
assert(Intl.getCanonicalLocales(['FR-fr', 'zh_CN']).join(',') === 'fr-FR,zh-CN',
       'and normalizes underscores');
assert(Intl.getCanonicalLocales().length === 0, 'undefined gives an empty list');

console.log('PASS: Intl polyfill — NumberFormat, PluralRules, Collator, ' +
            'DateTimeFormat, ListFormat, RelativeTimeFormat, DisplayNames');
