// The agent view (docs/agent-view.md) of a single document: `view` lists
// what an agent can see and operate, with handles that stay put across
// mutations; `--since` diffs; actions by handle answer with the diff.
// Driven in-process through controlCommand / controlResult, as
// test_agent_control.js does.

function run() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 600; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}
function ok() {
    var r = run.apply(null, arguments);
    assert(r.ok, Array.prototype.join.call(arguments, ' ') + ' succeeds: ' + r.payload);
    return r.payload;
}
// The line of the element whose quoted name is `name` ("" when none).
function lineOf(text, name) {
    var lines = text.split('\n');
    for (var i = 0; i < lines.length; i++)
        if (lines[i].indexOf('"' + name + '"') >= 0) return lines[i];
    return '';
}
function handleOf(text, name) {
    var l = lineOf(text, name).trim();
    assert(l, 'a line named "' + name + '" in:\n' + text);
    return l.split(' ')[0];
}
function rev(text) {
    var m = /^@view r(\d+)/.exec(text);
    assert(m, 'a view header: ' + text.split('\n')[0]);
    return 'r' + m[1];
}

document.title = 'Agent View';
document.body.innerHTML =
    '<style>body{margin:0;font:14px sans-serif} .gone{display:none} .clip{height:60px;overflow:auto}' +
    ' .clip li{height:30px} .ghost{visibility:hidden} .faded{opacity:0}</style>' +
    '<h1>Settings</h1>' +
    '<form aria-label="Account">' +
    '  <label for="user">User name</label><input id="user" value="ada">' +
    '  <label><input type="checkbox" id="remember" checked> Remember me</label>' +
    '  <select id="theme"><option>Light</option><option selected>Dark</option></select>' +
    '  <button id="save" type="button">Save</button>' +
    '  <button id="off" type="button" disabled>Delete</button>' +
    '</form>' +
    '<p id="note">Changes apply <b>at once</b>.</p>' +
    '<div class="gone"><button>Hidden by display</button></div>' +
    '<button class="ghost">Hidden by visibility</button>' +
    '<button class="faded">Hidden by opacity</button>' +
    '<button aria-hidden="true">Hidden from assistive tech</button>' +
    '<ul id="list"></ul>' +
    '<ul class="clip" id="scroller"><li>One</li><li>Two</li><li>Three</li><li>Four</li><li>Five</li></ul>' +
    '<div id="tile" style="cursor:pointer;width:120px;height:40px">Open tile</div>';
var list = document.getElementById('list');
for (var i = 1; i <= 60; i++) {
    var li = document.createElement('li');
    li.textContent = 'Item ' + i;
    li.style.height = '4px';
    li.style.fontSize = '3px';
    list.appendChild(li);
}
var tileClicks = 0;
document.getElementById('tile').addEventListener('click', function() { tileClicks++; });
advanceTime(32);

// ── a full view ─────────────────────────────────────────────────────────────

var v1 = ok('view');
console.log(v1);
assert(/^@view r\d+\n/.test(v1), 'a view starts with its revision');
assert(/^w0\* \S+ "Agent View" 0,0 \d+x\d+$/m.test(v1), 'the window line: w0, focused, title, rect');
assert(/^w0:$/m.test(v1), 'a section line before the elements');
assert(/ heading "Settings"$/m.test(v1), 'a heading');
assert(/ form "Account"$/m.test(v1), 'a named form is a group line');
assert(/ textbox "User name" ="ada"$/m.test(v1), 'a field: its label as name, its value');
assert(/ checkbox\* "Remember me"$/m.test(v1), 'a checked checkbox, named by its wrapping label');
assert(/ combobox "\w*" ="Dark"$/m.test(v1) || / combobox ="Dark"$/m.test(v1), 'a select shows its selection');
assert(/ button "Save"$/m.test(v1), 'a button');
assert(/ button! "Delete"$/m.test(v1), 'a disabled button is flagged !');
assert(/ text "Changes apply at once."$/m.test(v1), 'inline text is one line');
assert(v1.indexOf('User name"\n') < 0 || !/ text "User name"/.test(v1), 'a label gets no line of its own');
['Hidden by display', 'Hidden by visibility', 'Hidden by opacity', 'Hidden from assistive tech'].forEach(function(n) {
    assert(v1.indexOf(n) < 0, 'not shown: ' + n);
});
assert(/ list \[20 of 60\]$/m.test(v1), 'a long list shows 20 of its 60 items: ' + lineOf(v1, 'Item 1'));
assert(v1.indexOf('"Item 20"') >= 0 && v1.indexOf('"Item 21"') < 0, 'the first 20 items');
assert(v1.indexOf('"One"') >= 0 && v1.indexOf('"Five"') < 0, 'a scroll container shows what is scrolled into view');
assert(/ button "Open tile"$/m.test(v1), 'a pointer-cursor div with a click listener is a button');
assert(v1.split('\n').length < 80, 'the view is compact: ' + v1.split('\n').length + ' lines');

// ── paging a list ───────────────────────────────────────────────────────────

var listHandle = /^ *(\d+\.\d+) list \[20 of 60\]$/m.exec(v1)[1];
var page = ok('view', listHandle, '--from=40');
assert(page.indexOf('"Item 41"') >= 0 && page.indexOf('"Item 60"') >= 0 && page.indexOf('"Item 40"') < 0,
       'view N.M --from=40 pages the list:\n' + page);
assert(/ list \[41-60 of 60\]$/m.test(page), 'the page says where it is');

// ── no change, and handle stability ─────────────────────────────────────────

var r1 = rev(v1);
assert(ok('view', '--since=' + r1) === '@view ' + r1 + ' =\n', 'an idle view --since is one line');
var save = handleOf(v1, 'Save');
var user = handleOf(v1, 'User name');

// A mutation elsewhere: new element, removed element, changed value.
var extra = document.createElement('button');
extra.textContent = 'Added';
document.querySelector('form').insertBefore(extra, document.getElementById('save'));
document.getElementById('note').remove();
document.getElementById('user').value = 'grace';
document.getElementById('user').setAttribute('value', 'grace');
advanceTime(32);

var d = ok('view', '--since=' + r1);
console.log(d);
var r2 = rev(d);
assert(r2 !== r1, 'a change is a new revision');
assert(/^@view r\d+ \+1 -1 ~1$/m.test(d), 'the diff header counts one of each: ' + d.split('\n')[0]);
assert(/^\+ \d+\.\d+ button "Added"$/m.test(d), 'the added button');
assert(new RegExp('^- ' + handleOf(v1, 'Changes apply at once.').replace('.', '\\.') + '$', 'm').test(d),
       'the removed text, by handle');
assert(new RegExp('^~ ' + user.replace('.', '\\.') + ' ="grace"$', 'm').test(d), 'the changed value');

var v2 = ok('view');
assert(handleOf(v2, 'Save') === save && handleOf(v2, 'User name') === user, 'handles survive the mutations');

// Expired: a revision the server never had.
var e = ok('view', '--since=r999999');
assert(/^@view r\d+ full\n/.test(e), 'an unknown revision gets the full view');

// ── acting by handle ────────────────────────────────────────────────────────

var tile = handleOf(v2, 'Open tile');
var c = ok('click', tile);
assert(tileClicks === 1, 'click <handle> clicks it (' + tileClicks + ')');
assert(/^@view r\d+/.test(c), 'an action replies with a view header: ' + c);

// Clicking the field focuses it: the diff shows the focus flag.
var clickField = ok('click', user);
assert(new RegExp('^~ ' + user.replace('.', '\\.') + ' textbox@$', 'm').test(clickField),
       'the diff of a click shows the focus moving: ' + clickField);

// set replaces the value; type appends.
var setReply = ok('set', user, 'hopper');
assert(document.getElementById('user').value === 'hopper', 'set replaces: ' + document.getElementById('user').value);
assert(new RegExp('^~ ' + user.replace('.', '\\.') + ' ="hopper"$', 'm').test(setReply), 'set answers with the new value: ' + setReply);
ok('type', user, '!');
assert(document.getElementById('user').value === 'hopper!', 'type appends: ' + document.getElementById('user').value);
// key <handle>: the keys go to that element.
var keyed = ok('key', user, 'backspace');
assert(document.getElementById('user').value === 'hopper', 'key <handle> presses into it: ' + document.getElementById('user').value);
assert(new RegExp('^~ ' + user.replace('.', '\\.') + ' ="hopper"$', 'm').test(keyed), 'key answers with the diff: ' + keyed);

// A click that opens a dialog: the dialog is the answer.
document.getElementById('save').addEventListener('click', function() {
    var dlg = document.createElement('div');
    dlg.setAttribute('role', 'dialog');
    dlg.setAttribute('aria-label', 'Saved');
    dlg.innerHTML = '<p>All saved.</p><button id="okb">OK</button>';
    document.body.appendChild(dlg);
});
var opened = ok('click', save);
console.log(opened);
assert(/^\+ \d+\.\d+ dialog "Saved"$/m.test(opened), 'the dialog appears in the diff');
assert(/^\+ \d+\.\d+ text "All saved."$/m.test(opened) && /^\+ \d+\.\d+ button "OK"$/m.test(opened),
       'with its content');

// Checkbox toggles show as a flag change.
var remember = handleOf(opened.indexOf('Remember') >= 0 ? opened : ok('view'), 'Remember me');
var toggled = ok('click', remember);
assert(new RegExp('^~ ' + remember.replace('.', '\\.') + ' checkbox@?$', 'm').test(toggled) ||
       new RegExp('^~ ' + remember.replace('.', '\\.') + ' checkbox', 'm').test(toggled),
       'unchecking shows: ' + toggled);

// Focus is on the checkbox now.
var keys = ok('who-has-keys');
assert(/^w0\* /m.test(keys) && / checkbox@? ?"Remember me"/m.test(keys) || / checkbox[*]?@ "Remember me"/m.test(keys),
       'who-has-keys: the window and the focused element: ' + keys);

// An unknown handle fails with a reason.
var bad = run('click', '0.99999');
assert(!bad.ok && /no 0\.99999/.test(bad.payload), 'a stale handle fails: ' + bad.payload);

// The plain forms keep their JSON replies.
var plain = JSON.parse(ok('click', '5,5'));
assert(plain.pointer, 'click x,y still answers with the pointer');

// ── supporting commands ─────────────────────────────────────────────────────

assert(/^w0\* /.test(ok('windows')), 'windows lists w0');
var shot = JSON.parse(ok('shot', save));
assert(shot.width > 0 && shot.crop && shot.crop.width > 0, 'shot <handle> crops to the element');
