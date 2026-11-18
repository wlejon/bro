// Intl.Locale (js/intl_locale.js): parse, canonicalize, options, accessors,
// maximize / minimize, RangeError for a malformed tag; every expectation is
// V8's (node 26 / Chromium). Shoelace's LocalizeController makes one per
// render, and every Shoelace component with text threw "undefined is not a
// constructor".

const eq = (a, b, what) => assert(a === b, what + ': ' + JSON.stringify(a) + ' !== ' + JSON.stringify(b));

const l = new Intl.Locale('EN-us');
eq(l.toString(), 'en-US', 'canonical case');
eq(l.language, 'en', 'language');
eq(l.region, 'US', 'region');
eq(l.script, undefined, 'no script');
eq(l.baseName, 'en-US', 'baseName');
eq(Object.prototype.toString.call(l), '[object Intl.Locale]', 'toStringTag');
eq(JSON.stringify({ l }), '{"l":{}}', 'no toJSON, as in V8');

const m = new Intl.Locale('en', { region: 'GB', hourCycle: 'h23', numeric: true, calendar: 'gregory' });
eq(m.toString(), 'en-GB-u-ca-gregory-hc-h23-kn', 'options become sorted -u- keywords');
eq(m.hourCycle, 'h23', 'hourCycle');
eq(m.numeric, true, 'numeric');
eq(m.calendar, 'gregory', 'calendar');
eq(new Intl.Locale(m).toString(), m.toString(), 'a Locale is a tag');

eq(new Intl.Locale('zh').maximize().toString(), 'zh-Hans-CN', 'maximize');
eq(new Intl.Locale('en-Latn-US').minimize().toString(), 'en', 'minimize');
eq(new Intl.Locale('de-DE-u-co-phonebk-ca-buddhist').toString(), 'de-DE-u-ca-buddhist-co-phonebk', 'keywords sorted');
eq(new Intl.Locale('sr-Cyrl').script, 'Cyrl', 'script');

for (const t of ['x', 'en_US', '', 'en--US']) {
    let name = 'none';
    try { new Intl.Locale(t); } catch (e) { name = e.name; }
    eq(name, 'RangeError', 'malformed ' + JSON.stringify(t));
}
let typeErr = false;
try { new Intl.Locale(); } catch (e) { typeErr = e instanceof TypeError; }
assert(typeErr, 'no tag is a TypeError');
let rangeErr = false;
try { new Intl.Locale('en', { hourCycle: 'h99' }); } catch (e) { rangeErr = e instanceof RangeError; }
assert(rangeErr, 'a bad option value is a RangeError');
