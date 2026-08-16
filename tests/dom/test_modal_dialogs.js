// window.alert / confirm / prompt. Windowed runs put up a native modal box;
// headless has nobody to ask, so the message is logged and the dialog answers
// itself — setDialogAnswer() picks which way.

assert(typeof alert === 'function', 'alert exists');
assert(typeof confirm === 'function', 'confirm exists');
assert(typeof prompt === 'function', 'prompt exists');
assert(window.alert === alert, 'window.alert is the same function');
assert(window.confirm === confirm, 'window.confirm is the same function');
assert(window.prompt === prompt, 'window.prompt is the same function');

// Default: accept. A script driving an app walks through its confirmations
// instead of stopping at the first one.
assert(alert('a message') === undefined, 'alert returns undefined');
assert(confirm('proceed?') === true, 'confirm accepts by default');
assert(prompt('name?', 'Jonny') === 'Jonny', 'prompt returns its default');
assert(prompt('name?') === '', 'prompt with no default returns the empty string');

// Cancel branch.
setDialogAnswer(false);
assert(confirm('proceed?') === false, 'confirm cancels when told to');
assert(prompt('name?', 'Jonny') === null, 'prompt returns null when cancelled');

setDialogAnswer(true);
assert(confirm('proceed?') === true, 'confirm accepts again');

// Non-string arguments stringify, as in a browser.
assert(confirm(42) === true, 'a number message is fine');
assert(confirm() === true, 'no message at all is fine');
