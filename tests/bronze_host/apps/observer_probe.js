// The observer probe: MutationObserver over the DOM layer's own notices.
//
// The claim worth testing is not "records arrive". It is WHERE they come from.
// This layer could have watched its own mutators — appendChild, setAttribute
// and the rest all pass through host_element.cpp and host_node.cpp — and every
// assertion below except one would still pass. The one is `page.*`: a script in
// the page's QuickJS realm sets an attribute, and the compiled observer has to
// see it. That only works because the notice is fired by dom::Element itself
// (Document::notifyMutation), which is the point of putting it there.
//
// The other three claims:
//
//   1. Records are BATCHED. Six mutations in one turn are one callback with six
//      records, in mutation order — not six callbacks. Code that rebuilds a
//      view from records depends on seeing them together.
//
//   2. Delivery is ASYNCHRONOUS. Nothing is delivered inside the turn that
//      mutated, which is what lets a run of mutations be coalesced at all.
//
//   3. Scope is honoured in both directions. An observer without `subtree`
//      must not hear about a grandchild, and one with it must.
//
// WHY THE RECORDS ARE FLATTENED TO STRINGS. A record is eight fields and there
// are more than a dozen of them here; printing each field on its own line would
// bury the shape in noise. `summary()` below prints one line per record —
// type, target, added/removed counts, attribute, old value — so a wrong record
// shows up as one wrong line at the position it occurred, which is also what
// makes the mutation ORDER part of what is pinned.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

const got = {};

// A node, named so a record reads back legibly. `id` where there is one,
// because that is what the fixture and this file agree on.
function name(n) {
    if (n === null || n === undefined) return '-';
    if (n.nodeType === 3) return '#text';
    if (n.nodeType === 8) return '#comment';
    return n.id ? '#' + n.id : 'el';
}

function summary(r) {
    return r.type +
           ' on ' + name(r.target) +
           ' +' + r.addedNodes.length + '(' + (r.addedNodes.length ? name(r.addedNodes[0]) : '-') + ')' +
           ' -' + r.removedNodes.length + '(' + (r.removedNodes.length ? name(r.removedNodes[0]) : '-') + ')' +
           ' prev=' + name(r.previousSibling) +
           ' next=' + name(r.nextSibling) +
           ' attr=' + (r.attributeName === null ? '-' : r.attributeName) +
           ' old=' + (r.oldValue === null ? '-' : r.oldValue);
}

const host = document.getElementById('host');
const shared = document.getElementById('shared');
say('fixture.hostFound', host !== null);
say('fixture.sharedFound', shared !== null);
say('fixture.hostChildren', host.childNodes.length);

// ---------------------------------------------------------------------------
// One observer over everything under #host
// ---------------------------------------------------------------------------

const lines = [];
let batches = 0;
let observerArgIsSelf = false;
let deliveredSynchronously = false;
let turnOver = false;

const main = new MutationObserver(function (records, observer) {
    if (!turnOver) deliveredSynchronously = true;
    batches++;
    observerArgIsSelf = observer === main;
    for (const r of records) lines.push(summary(r));
});
main.observe(host, {
    childList: true, subtree: true, attributes: true, characterData: true,
    attributeOldValue: true, characterDataOldValue: true,
});

// Six mutations, one turn. The seventh statement here — setting an id on a
// DETACHED element — must NOT produce a record: it is not under #host yet, and
// an observer that reported it would be reporting on a tree it does not watch.
const box = document.createElement('div');
box.id = 'box';

host.appendChild(box);                  // 1 childList, prev = #second
box.setAttribute('data-k', 'v1');       // 2 attributes, no old value
box.setAttribute('data-k', 'v2');       // 3 attributes, old = v1
const text = document.createTextNode('hi');
box.appendChild(text);                  // 4 childList on #box
text.data = 'bye';                      // 5 characterData, old = hi
host.removeChild(box);                  // 6 childList, removed, prev = #second

// Each of the three below gets its own container, because `main` is watching
// #host with subtree and would otherwise collect every scratch element the
// other tests append — which would make its expectation a record of the order
// the tests happen to run in.

// ---------------------------------------------------------------------------
// Scope: an observer WITHOUT subtree must not hear about a grandchild
// ---------------------------------------------------------------------------

const penScope = document.getElementById('pen_scope');
let narrowCount = 0;
const narrow = new MutationObserver(function (records) { narrowCount += records.length; });
narrow.observe(penScope, { childList: true });

const outer = document.createElement('div');
penScope.appendChild(outer);            // counted: a direct child
const inner = document.createElement('span');
outer.appendChild(inner);               // NOT counted: one level too deep

// ---------------------------------------------------------------------------
// disconnect: pending records go with it, and nothing arrives after
// ---------------------------------------------------------------------------

const penGone = document.getElementById('pen_gone');
let goneCount = 0;
const gone = new MutationObserver(function (records) { goneCount += records.length; });
gone.observe(penGone, { childList: true });
penGone.appendChild(document.createElement('div'));   // queued...
gone.disconnect();                                    // ...and dropped
penGone.appendChild(document.createElement('div'));   // never seen

// ---------------------------------------------------------------------------
// takeRecords: the queue, synchronously, and emptied
// ---------------------------------------------------------------------------

const penTake = document.getElementById('pen_take');
let takeDelivered = 0;
const taker = new MutationObserver(function (records) { takeDelivered += records.length; });
taker.observe(penTake, { childList: true });
penTake.appendChild(document.createElement('div'));
const takenCount = taker.takeRecords().length;
const takenAgain = taker.takeRecords().length;

// ---------------------------------------------------------------------------
// The cross-realm case: the PAGE's script sets an attribute on #shared
// ---------------------------------------------------------------------------

const page = new MutationObserver(function (records) {
    for (const r of records) {
        got['page.record'] = r.type + ':' + r.attributeName + ':' + name(r.target);
        got['page.value'] = r.target.getAttribute('data-page');
    }
});
page.observe(shared, { attributes: true });

// The page's listener runs synchronously inside this dispatch, in the other
// realm, and sets the attribute. Nothing of this layer's is on that path — the
// record can only exist because dom::Element fired the notice itself.
document.dispatchEvent({ type: 'app:mutatePage' });

turnOver = true;

// ---------------------------------------------------------------------------
// Report, several frames later
// ---------------------------------------------------------------------------
// Late enough for the page's 20 ms timeout to have run at one frame per 16 ms
// and for its record to have been delivered on the frame after that.

let frames = 0;
function tick() {
    if (++frames < 5) { requestAnimationFrame(tick); return; }

    say('sync.deliveredInTurn', deliveredSynchronously);
    say('main.batches', batches);
    say('main.observerArg', observerArgIsSelf);
    say('main.records', lines.length);
    for (let i = 0; i < lines.length; i++) say('rec' + i, lines[i]);

    say('narrow.count', narrowCount);
    say('gone.count', goneCount);
    say('take.count', takenCount);
    say('take.again', takenAgain);
    say('take.delivered', takeDelivered);

    say('page.record', got['page.record'] === undefined ? 'absent' : got['page.record']);
    say('page.value', got['page.value'] === undefined ? 'absent' : got['page.value']);

    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
}
requestAnimationFrame(tick);
