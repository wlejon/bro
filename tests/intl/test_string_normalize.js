// String.prototype.normalize (bronze's runtime, UAX #15): it answered the
// string unchanged, so the common accent fold, normalize('NFD') and the
// combining marks dropped, did nothing ('Crème' stayed 'Crème') and an app
// matching names typed without accents could not.

const fold = (s) => s.normalize('NFD').replace(/\p{M}/gu, '');
assert(fold('Crème Brûlée') === 'Creme Brulee', 'NFD and the marks dropped fold the accents: ' + fold('Crème Brûlée'));
assert(fold('Pájaro Carpintero') === 'Pajaro Carpintero', 'and Spanish');

assert('é'.normalize('NFD') === 'é', 'é decomposes');
assert('é'.normalize('NFC') === 'é', 'and composes back');
assert('é'.normalize() === 'é', 'NFC is the default');
assert('ạ̇'.normalize('NFD') === 'ạ̇', 'marks are put in canonical order');
assert('Å'.normalize('NFC') === 'Å', 'the angstrom sign is Å');
assert('क़'.normalize('NFC') === 'क़', 'an excluded composition stays decomposed');
assert('가'.normalize('NFD') === '가' && '가'.normalize('NFC') === '가', 'Hangul both ways');
assert('ﬁ①'.normalize('NFKC') === 'fi1', 'compatibility forms: ligature and circled digit');
assert('ﬁ'.normalize('NFC') === 'ﬁ', 'which canonical forms keep');
assert('\uD834'.normalize('NFC') === '\uD834', 'a lone surrogate passes through');
const ascii = 'plain ascii';
assert(ascii.normalize('NFD') === ascii, 'ASCII is unchanged');

let threw = null;
try { 'x'.normalize('bogus'); } catch (e) { threw = e; }
assert(threw instanceof RangeError, 'an unknown form throws a RangeError');

console.log('OK — String.prototype.normalize: NFC, NFD, NFKC, NFKD');
