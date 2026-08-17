// The abort probe: AbortController, AbortSignal, and the fetch that obeys one.
//
// A signal is the only object in this layer whose whole job is to say NO to
// work already under way, so the assertions that matter are the negative ones:
// a fetch that must not deliver, a listener that must not fire twice, a reason
// that must not be replaced by a later abort. Each of those failing looks like
// success from every other angle — the fetch resolves, the app renders, and the
// user sees the texture they navigated away from.
//
// THREE CLAIMS THIS FILE EXISTS TO PIN:
//
//   1. Abort is idempotent and first-writer-wins. `reason` is set once; the
//      second abort() fires nothing and changes nothing. Cleanup paths call
//      abort() twice all the time, and a signal that re-fired would run every
//      listener again against state that has already been torn down.
//
//   2. A signal carries its abort across the frame boundary that fetch settles
//      on. `fetch(url, {signal})` followed by `signal.abort()` in the SAME turn
//      must reject: the read has not happened yet, and this is the whole of
//      cancellation here (src/bronze_host/host_fetch.cpp says so).
//
//   3. The host does not trust `signal.aborted`. It is an ordinary writable
//      property — the app can assign it — and the host decides on its own copy
//      in the payload struct. `guard.*` below writes false onto an aborted
//      signal and the fetch still rejects.
//
// WHY THE ASYNC HALF RECORDS RATHER THAN PRINTS: five independent promise
// chains, and the order their lines would interleave is a property of how many
// microtask hops each takes rather than of anything this layer promises. Values
// are collected and printed in a fixed order from the final frame; the
// sequencing claims that DO matter are asserted directly. Same reasoning as
// file_probe.js, which spells it out at length.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// ---------------------------------------------------------------------------
// A fresh controller
// ---------------------------------------------------------------------------

const c1 = new AbortController();
say('fresh.aborted', c1.signal.aborted);
say('fresh.reasonUndefined', c1.signal.reason === undefined);
say('fresh.signalIsStable', c1.signal === c1.signal);   // one object, not a rebuild

// ---------------------------------------------------------------------------
// Aborting: the event, the reason, and the fact that it happens once
// ---------------------------------------------------------------------------

let fired = 0;
let sawType = '';
let sawTargetIsSignal = false;

c1.signal.onabort = function (evt) {
    fired++;
    sawType = evt.type;
    sawTargetIsSignal = evt.target === c1.signal;
};
c1.signal.addEventListener('abort', function () { fired++; });

c1.abort();
say('abort.aborted', c1.signal.aborted);
say('abort.reasonName', c1.signal.reason.name);
say('abort.fired', fired);              // onabort + one listener
say('abort.evtType', sawType);
say('abort.evtTarget', sawTargetIsSignal);

// Second abort: nothing fires, and the first reason survives. A cleanup path
// that calls abort() twice is the common case, not the exotic one.
c1.abort('a different reason');
say('abort.firedAfterSecond', fired);
say('abort.reasonKept', c1.signal.reason.name);

// A listener added after the fact never runs: the abort event has already
// fired, and a signal does not replay it.
c1.signal.addEventListener('abort', function () { fired++; });
say('abort.firedAfterLate', fired);

// ---------------------------------------------------------------------------
// A reason the app chose
// ---------------------------------------------------------------------------

const c2 = new AbortController();
c2.abort('user navigated away');
say('reason.custom', c2.signal.reason);

// throwIfAborted throws the reason ITSELF, untouched — which is what makes
// `catch (e) { if (e === myToken) }` and `e.name === 'AbortError'` both work.
let thrownFresh = 'none';
try { new AbortController().signal.throwIfAborted(); }
catch (e) { thrownFresh = 'threw'; }
say('throwIf.fresh', thrownFresh);

let thrownAborted = 'none';
try { c2.signal.throwIfAborted(); }
catch (e) { thrownAborted = e; }
say('throwIf.aborted', thrownAborted);

// ---------------------------------------------------------------------------
// The statics — a namespace here, because a host function cannot carry one
// (src/bronze_host/host_abort.cpp quotes the runtime message)
// ---------------------------------------------------------------------------

const preAborted = AbortSignal.abort();
say('static.abortAborted', preAborted.aborted);
say('static.abortReason', preAborted.reason.name);
say('static.abortCustom', AbortSignal.abort('nope').reason);

// AbortSignal.any: the first source to abort wins, and carries its reason.
const s1 = new AbortController();
const s2 = new AbortController();
const any = AbortSignal.any([s1.signal, s2.signal]);
say('any.freshAborted', any.aborted);
s2.abort('second');
say('any.aborted', any.aborted);
say('any.reason', any.reason);
s1.abort('first');
say('any.reasonKept', any.reason);   // still the first abort's

// A source that is ALREADY aborted aborts the composite at construction.
const any2 = AbortSignal.any([AbortSignal.abort('pre'), new AbortController().signal]);
say('any.preAborted', any2.aborted);
say('any.preReason', any2.reason);

// A deadline signal, which is the reason hostSetTimeout exists.
const timed = AbortSignal.timeout(30);
say('timeout.freshAborted', timed.aborted);

// ---------------------------------------------------------------------------
// Everything below is asynchronous, so it RECORDS rather than prints
// ---------------------------------------------------------------------------

const got = {};

function record(name, value) { got[name] = value; }

// The control: a signal that never aborts must not disturb an ordinary fetch.
// Without this line every assertion below would also pass against a fetch that
// had simply stopped working.
fetch('note.txt', { signal: new AbortController().signal })
    .then(function (r) { return r.text(); })
    .then(function (t) { record('fetch.control', t); });

// Already aborted before the call: the file is never read.
fetch('note.txt', { signal: AbortSignal.abort() })
    .then(function () { record('fetch.pre', 'RESOLVED'); })
    .catch(function (e) { record('fetch.pre', e.name); });

// Aborted after the call, in the same turn — claim 2 above.
const c3 = new AbortController();
fetch('note.txt', { signal: c3.signal })
    .then(function () { record('fetch.late', 'RESOLVED'); })
    .catch(function (e) { record('fetch.late', e.name); });
c3.abort();

// Aborting a signal whose fetch has already settled changes nothing: no throw,
// no second delivery, and the value the chain already had stands.
const c4 = new AbortController();
fetch('note.txt', { signal: c4.signal })
    .then(function (r) { return r.text(); })
    .then(function (t) {
        record('fetch.settled', t);
        c4.abort();
        record('fetch.abortAfterSettle', 'harmless');
    })
    .catch(function (e) { record('fetch.settled', 'REJECTED:' + e.name); });

// Claim 3: `aborted` is the app's property to write, and writing it does not
// change what the host decides.
const c5 = new AbortController();
c5.abort();
c5.signal.aborted = false;
say('guard.propWrite', c5.signal.aborted);
fetch('note.txt', { signal: c5.signal })
    .then(function () { record('guard.fetch', 'RESOLVED'); })
    .catch(function (e) { record('guard.fetch', e.name); });

// A fetch with no signal at all still works — the argument is optional, and a
// missing one must not read as an aborted one.
fetch('note.txt').then(function (r) { return r.text(); })
                 .then(function (t) { record('fetch.noSignal', t); });

// ---------------------------------------------------------------------------
// Done, several frames later
// ---------------------------------------------------------------------------
// Late enough for the 30 ms deadline above to have passed at one frame per
// 16 ms, and for every fetch chain to have settled. A value still missing at
// that point prints as `absent`, which is a visible failure rather than a hang.

const REPORT = [
    'fetch.control', 'fetch.pre', 'fetch.late', 'fetch.settled',
    'fetch.abortAfterSettle', 'guard.fetch', 'fetch.noSignal',
];

let frames = 0;
function tick() {
    if (++frames < 5) { requestAnimationFrame(tick); return; }
    say('timeout.aborted', timed.aborted);
    say('timeout.reasonName', timed.reason.name);
    for (const name of REPORT) {
        say(name, Object.prototype.hasOwnProperty.call(got, name) ? got[name] : 'absent');
    }
    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
}
requestAnimationFrame(tick);
