// The form surface: the HTMLFormElement members, the control reflections, and
// constraint validation.
//
// Regression: the bronze port had none of `form.elements`, `submit()`,
// `reset()`, `requestSubmit()`, `input.form`, `maxLength`, `minLength`,
// `pattern` or `size`, and its `checkValidity()` on a <form> ran the CONTROL
// check against the form itself — which has no value and no constraints, so it
// always answered true and `validity` reported nothing at all.

const root = document.getElementById('root');

root.innerHTML =
    '<form id="f">' +
    '  <input name="email" type="email" required maxlength="40" minlength="3"' +
    '         pattern="[^@]+@example\\.com" size="30">' +
    '  <input name="age" type="number" min="10" max="20" step="5">' +
    '  <textarea name="bio"></textarea>' +
    '  <select name="pick"><option value="a">a</option>' +
    '    <option value="b" selected>b</option></select>' +
    '  <button name="go" type="submit">go</button>' +
    '</form>' +
    '<input id="outside" name="outside" form="f">';
flush();

const form = document.getElementById('f');
const email = form.elements.email;
const age = form.elements.age;

// --- form.elements --------------------------------------------------------
assert(form.elements.length === 6,
       'form.elements collects every owned control: ' + form.elements.length);
assert(email.tagName === 'INPUT', 'form.elements is indexable by name');
assert(form.elements[0] === email, 'form.elements is indexable by position');
assert(form.elements.outside === document.getElementById('outside'),
       'form="id" associates a control that lives outside the form');

// --- input.form -----------------------------------------------------------
assert(email.form === form, 'a control reports its ancestor form');
assert(document.getElementById('outside').form === form,
       'form="id" is what `form` reports for an out-of-tree control');
const orphan = document.createElement('input');
assert(orphan.form === null, 'a control with no form reports null');

// --- the numeric and string reflections -----------------------------------
assert(email.maxLength === 40, 'maxLength reflects maxlength: ' + email.maxLength);
assert(email.minLength === 3, 'minLength reflects minlength: ' + email.minLength);
assert(email.size === 30, 'size reflects size: ' + email.size);
assert(email.pattern === '[^@]+@example\\.com', 'pattern reflects pattern');
assert(age.maxLength === -1, 'maxLength with no attribute is -1: ' + age.maxLength);
assert(age.size === 0, 'size with no attribute is 0: ' + age.size);
email.maxLength = 12;
assert(email.getAttribute('maxlength') === '12', 'maxLength writes the attribute');
email.maxLength = 40;

// --- validity -------------------------------------------------------------
assert(email.willValidate === true, 'a text input will validate');
assert(document.getElementById('f').elements.go.willValidate === false,
       'a submit button does not validate');

assert(email.validity.valueMissing === true, 'an empty required field is missing');
assert(email.validity.valid === false, 'and therefore invalid');
assert(email.validationMessage.length > 0, 'an invalid control has a message');

email.value = 'nope';
assert(email.validity.valueMissing === false, 'a filled field is not missing');
assert(email.validity.typeMismatch === true, 'a non-address fails type=email');

email.value = 'me@other.org';
assert(email.validity.typeMismatch === false, 'an address passes type=email');
assert(email.validity.patternMismatch === true, 'the wrong domain fails the pattern');

email.value = 'me@example.com';
assert(email.validity.patternMismatch === false, 'the right domain passes the pattern');
assert(email.validity.valid === true, 'and the control is now valid');
assert(email.validationMessage === '', 'a valid control has no message');

email.value = 'a@example.com';
email.minLength = 20;
assert(email.validity.tooShort === true, 'minlength is enforced');
email.minLength = 3;
email.maxLength = 5;
assert(email.validity.tooLong === true, 'maxlength is enforced');
email.maxLength = 40;
assert(email.validity.valid === true, 'back to valid');

// minlength counts UTF-16 units, not bytes.
const u = document.createElement('input');
u.setAttribute('minlength', '4');
form.appendChild(u);
u.value = 'unicode';
assert(u.validity.tooShort === false, 'a long enough value is not too short');
u.value = 'ün';
assert(u.validity.tooShort === true, 'two characters is under a minlength of 4');
u.value = 'ünïcø';
assert(u.validity.tooShort === false,
       'minlength counts characters, not UTF-8 bytes');
form.removeChild(u);

// number range + step
age.value = '5';
assert(age.validity.rangeUnderflow === true, 'below min is an underflow');
age.value = '25';
assert(age.validity.rangeOverflow === true, 'above max is an overflow');
age.value = '13';
assert(age.validity.stepMismatch === true, 'off-step is a step mismatch');
age.value = '15';
assert(age.validity.valid === true, 'on-step inside the range is valid');
age.value = 'abc';
assert(age.validity.badInput === true, 'unparseable text in a number is bad input');
age.value = '15';

// setCustomValidity
age.setCustomValidity('nope');
assert(age.validity.customError === true, 'setCustomValidity marks a custom error');
assert(age.validationMessage === 'nope', 'the custom message is what is reported');
assert(age.checkValidity() === false, 'a custom error fails checkValidity');
age.setCustomValidity('');
assert(age.validity.customError === false, 'an empty string clears it');

// A required radio group is satisfied by any member.
const radios = document.createElement('div');
root.appendChild(radios);
radios.innerHTML = '<form id="rf"><input id="ra" type="radio" name="g" required>' +
                   '<input id="rb" type="radio" name="g" required checked></form>';
flush();
assert(document.getElementById('ra').validity.valueMissing === false,
       'a required radio is satisfied by any member of its group being checked');

// --- form-wide checkValidity and the invalid events -----------------------
const bio = form.elements.bio;
bio.setAttribute('required', '');
let invalids = [];
const owned = form.elements;
for (let i = 0; i < owned.length; i++) {
    owned[i].addEventListener('invalid', function () { invalids.push(1); });
}
assert(form.checkValidity() === false,
       'a form with an invalid control fails checkValidity');
assert(invalids.length === 1,
       'exactly the invalid control got an invalid event: ' + invalids.length);
assert(form.reportValidity() === false, 'reportValidity agrees');
bio.removeAttribute('required');
invalids = [];
assert(form.checkValidity() === true, 'a form with no invalid control passes');
assert(invalids.length === 0, 'and nothing got an invalid event');

// --- requestSubmit / submit / reset ---------------------------------------
let submits = 0, lastSubmitter = null;
form.addEventListener('submit', function (e) {
    submits++;
    lastSubmitter = e.submitter;
    e.preventDefault();
});
form.requestSubmit();
assert(submits === 1, 'requestSubmit fires submit');
assert(lastSubmitter === null || lastSubmitter === undefined,
       'requestSubmit with no argument has no submitter');
form.requestSubmit(form.elements.go);
assert(submits === 2, 'requestSubmit fires again');
assert(lastSubmitter === form.elements.go, 'the submitter is the one passed');

// requestSubmit validates first.
bio.setAttribute('required', '');
form.requestSubmit();
assert(submits === 2, 'requestSubmit on an invalid form does not fire submit');
bio.removeAttribute('required');

// submit() skips both validation and the event, per spec.
form.submit();
assert(submits === 2, 'submit() fires no submit event');

// reset() fires reset and restores the select's default option.
const pick = form.elements.pick;
pick.selectedIndex = 0;
let resets = 0;
form.addEventListener('reset', function () { resets++; });
form.reset();
assert(resets === 1, 'reset() fires reset');
assert(pick.selectedIndex === 1,
       'reset() restores the option carrying [selected]: ' + pick.selectedIndex);

root.innerHTML = '';
