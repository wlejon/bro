// A radio group is one control, however the choice is made.
//
// HTML: when a radio's checkedness becomes true "for whatever reason", every
// other member of its group must become false. bro only did this on a click, so
// code that restored a saved selection with `input.checked = true` left BOTH
// radios ticked — a group showing two answers to a one-answer question.
//
// The group is: same tree, same form owner, type=radio, same non-empty name.
// Scope matters as much as the rule: two forms may each hold a "size" group
// without either one steering the other.

document.body.innerHTML =
    '<input type="radio" name="g" id="a" checked>' +
    '<input type="radio" name="g" id="b">' +
    '<input type="radio" name="g" id="c">' +
    '<input type="radio" name="other" id="d">' +
    '<input type="radio" id="nameless1">' +
    '<input type="radio" id="nameless2" checked>' +
    '<form id="f1"><input type="radio" name="g" id="fa" checked>' +
                  '<input type="radio" name="g" id="fb"></form>' +
    '<form id="f2"><input type="radio" name="g" id="ga" checked></form>';
flush();

const $ = (id) => document.getElementById(id);
const on = (...ids) => ids.filter((i) => $(i).checked).join(',');

assert(on('a', 'b', 'c') === 'a', 'the parsed [checked] radio starts on');

// ── the assignment path — the one that was broken ─────────────────────────
$('b').checked = true;
assert(on('a', 'b', 'c') === 'b',
       'assigning .checked cleared the group, got ' + on('a', 'b', 'c'));

$('c').checked = true;
assert(on('a', 'b', 'c') === 'c',
       'and again from a different member, got ' + on('a', 'b', 'c'));

// Assigning false clears only that one — a group may legitimately have nothing
// picked, and unchecking is not a choice of anything else.
$('c').checked = false;
assert(on('a', 'b', 'c') === '', 'assigning false leaves the group empty');

// Assigning true to an ALREADY-checked radio is still a set to true, and must
// leave the rest of the group off rather than being skipped as a no-op.
$('a').checked = true;
$('b').checked = true;
$('b').checked = true;
assert(on('a', 'b', 'c') === 'b', 're-asserting the same radio keeps the group at one');

// ── the click paths ───────────────────────────────────────────────────────
$('a').click();
assert(on('a', 'b', 'c') === 'a', 'element.click() clears the group');

const r = $('c').getBoundingClientRect();
click(r.left + r.width / 2, r.top + r.height / 2);
flush();
assert(on('a', 'b', 'c') === 'c', 'a hit-tested click clears the group');

// ── group membership ──────────────────────────────────────────────────────
// A different name is a different group.
$('d').checked = true;
assert($('c').checked === true, 'another name is another group');
assert($('d').checked === true, 'and it checked');

// A nameless radio is a group of one: checking it must not unpick every other
// nameless radio on the page.
$('nameless1').checked = true;
assert($('nameless2').checked === true,
       'an unnamed radio does not drag other unnamed radios with it');

// Same name, different form owner: independent groups. This is what makes the
// scope rule visible — a document-wide sweep would clear all three at once.
assert(on('fa', 'ga') === 'fa,ga', 'each form starts with its own pick');
$('fb').checked = true;
assert($('fb').checked === true && $('fa').checked === false,
       'the pick moved within form 1');
assert($('ga').checked === true, "form 2's pick is untouched by form 1");
assert($('c').checked === true, 'and so is the formless group');

// The formless group likewise cannot reach into a form.
$('a').checked = true;
assert($('fb').checked === true && $('ga').checked === true,
       'a formless radio does not clear radios inside forms');

// ── no events from the setter ─────────────────────────────────────────────
// Per spec only user interaction fires change/input; the IDL setter is silent.
// App code that assigns .checked dispatches its own event, and a duplicate from
// the engine would run every handler twice.
let fired = 0;
['change', 'input'].forEach((t) => {
    $('a').addEventListener(t, () => { fired++; });
    $('b').addEventListener(t, () => { fired++; });
});
$('b').checked = true;
assert(fired === 0, 'assigning .checked fires nothing, got ' + fired + ' events');
$('a').click();
assert(fired === 2, 'a click still fires change + input, got ' + fired);

console.log('PASS radio group exclusivity');
