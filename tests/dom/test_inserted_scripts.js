// Script elements inserted by script run, as in Chromium (HTML "prepare the
// script element"): an inline classic script at once, inside the call that
// connected it (jQuery's globalEval appends one and removes it again); one
// with a src on a later task, then `load`, or `error` when it cannot be had.
// A script runs once; innerHTML's scripts never run, a contextual
// fragment's do; one not connected waits until it is; a data block or a
// module script is not run here.

window.R = [];

// Inline: synchronous, and only once.
const s1 = document.createElement('script');
s1.textContent = "R.push('inline')";
document.head.appendChild(s1);
assert(R.join() === 'inline', 'an inline script runs inside appendChild, got ' + R.join());
document.body.appendChild(s1);
assert(R.join() === 'inline', 'moving a script that ran does not run it again');
document.head.removeChild(s1);

// jQuery's globalEval: append, then remove at once; the code ran.
const s2 = document.createElement('script');
s2.text = "window.evalled = 42";
document.head.appendChild(s2).parentNode.removeChild(s2);
assert(window.evalled === 42, 'globalEval-style insertion ran its code');

// A detached script waits for its connection, including one inside a subtree.
const wrap = document.createElement('div');
const s3 = document.createElement('script');
s3.textContent = "R.push('in subtree')";
wrap.appendChild(s3);
assert(R.length === 1, 'a script in a detached subtree does not run');
document.body.appendChild(wrap);
assert(R.join() === 'inline,in subtree', 'connecting its subtree runs it, got ' + R.join());

// innerHTML's scripts never run; a contextual fragment's do.
const host = document.createElement('div');
document.body.appendChild(host);
host.innerHTML = "<script>R.push('innerHTML')<\/script>";
document.body.appendChild(host.firstChild);
assert(!R.includes('innerHTML'), 'an innerHTML script never runs, even when moved');
const frag = document.createRange().createContextualFragment("<script>R.push('fragment')<\/script>");
document.body.appendChild(frag);
assert(R.includes('fragment'), 'a contextual fragment script runs when inserted');

// Not JavaScript: left alone.
const data = document.createElement('script');
data.type = 'application/json';
data.textContent = "R.push('json')";
document.body.appendChild(data);
assert(!R.includes('json'), 'a data block is not run');

// A throw is reported and the inserting call returns normally.
let reported = null;
window.onerror = (msg) => { reported = String(msg); return true; };
const bad = document.createElement('script');
bad.textContent = "throw new Error('inserted boom')";
document.body.appendChild(bad);
assert(reported && reported.includes('inserted boom'), 'the throw reached window.onerror, got ' + reported);
window.onerror = null;

// External: runs on a later task, then load; a missing file fires error.
const ev = [];
const ok = document.createElement('script');
ok.src = 'inserted_script/ok.js';  // tests/test_app/inserted_script/ok.js
ok.onload = (e) => ev.push('load ' + e.bubbles);
ok.onerror = () => ev.push('error ok?');
document.head.appendChild(ok);
assert(!R.includes('external ran'), 'an external script does not run inside appendChild');
const missing = document.createElement('script');
missing.src = 'inserted_script/missing.js';
missing.addEventListener('error', (e) => ev.push('error ' + e.isTrusted));
document.head.appendChild(missing);
advanceTime(50);
assert(R.includes('external ran'), 'the external script ran on a later task, got ' + R.join());
assert(ev.join() === 'load false,error true', 'load after it ran, error for the missing one, got ' + ev.join());

console.log('test_inserted_scripts: OK');
