// <input pattern=...> must use the ECMAScript RegExp dialect (compiled with
// the 'u' flag — the spec-sanctioned approximation of 'v'), implicitly anchored as ^(?:pattern)$, and
// matched over the UTF-16 form of the value. The engine is QuickJS's own
// libregexp — the dialect-divergent cases below (named groups, lookbehind,
// \u{...}) all failed or mis-matched under the previous std::regex build.

const root = document.getElementById('root');
root.innerHTML = '<input id="p" type="text">';
flush();
const p = document.getElementById('p');

function check(pattern, value) {
    p.setAttribute('pattern', pattern);
    p.value = value;
    const v = p.validity;
    // patternMismatch and valid must agree (no other constraint is active).
    assert(v.patternMismatch === !v.valid,
           'patternMismatch/valid coherent for ' + pattern + ' vs ' + JSON.stringify(value));
    return !v.patternMismatch;
}

// ---- basics work in both dialects ----------------------------------------
assert(check('\\d{2,4}', '123'), 'digits match');
assert(!check('\\d{2,4}', '1'), 'too few digits mismatch');
assert(!check('\\d{2,4}', 'abc'), 'letters mismatch');

// ---- implicit anchoring ---------------------------------------------------
assert(check('ab', 'ab'), 'exact match');
assert(!check('ab', 'xaby'), 'pattern must match the WHOLE value (anchored)');
assert(!check('ab', 'abb'), 'trailing extra mismatches');

// ---- ECMAScript-only syntax (std::regex rejected these, wrongly ignoring
// ---- the constraint — dialect-divergent proof cases) ----------------------
assert(check('(?<y>\\d+)', '12'), 'named group matches digits');
assert(!check('(?<y>\\d+)', 'ab'), 'named group mismatches letters');
assert(check('a(?<=a)b', 'ab'), 'lookbehind matches');
assert(!check('a(?<=a)b', 'xb'), 'lookbehind pattern mismatches other value');
assert(check('\\u{1F600}', '\u{1F600}'), 'unicode escape matches astral char');
assert(!check('\\u{1F600}', 'x'), 'unicode escape mismatches other value');

// ---- UTF-16 / unicode-mode subject ---------------------------------------
assert(check('.', 'é'), 'dot matches one non-ASCII char (not per-byte)');
assert(check('.', '\u{1F600}'), 'dot matches one astral code point (unicode mode)');
assert(check('[\\u{1F300}-\\u{1FAFF}]', '\u{1F600}'), 'astral class range matches');
assert(check('日本語', '日本語'), 'literal CJK pattern matches');
assert(!check('日本語', '日本'), 'literal CJK prefix mismatches');

// ---- invalid pattern → constraint ignored (element stays valid) -----------
assert(check('(', 'anything'), 'unclosed group: pattern ignored, valid');
assert(check('a{2,1}', 'zzz'), 'bad quantifier range: pattern ignored, valid');

// ---- empty value never mismatches ----------------------------------------
p.setAttribute('pattern', '\\d+');
p.value = '';
assert(p.validity.patternMismatch === false, 'empty value: no patternMismatch');
assert(p.validity.valid === true, 'empty value with pattern only: valid');

// ---- checkValidity + validationMessage reflect the pattern ----------------
p.setAttribute('pattern', '\\d+');
p.value = 'abc';
let invalidFired = false;
p.addEventListener('invalid', () => { invalidFired = true; });
assert(p.checkValidity() === false, 'checkValidity false on mismatch');
assert(invalidFired, 'invalid event fired');
assert(p.validationMessage.length > 0, 'validationMessage non-empty on mismatch');
p.value = '42';
assert(p.checkValidity() === true, 'checkValidity true on match');
assert(p.validationMessage === '', 'validationMessage empty when valid');

console.log('test_input_pattern OK');
