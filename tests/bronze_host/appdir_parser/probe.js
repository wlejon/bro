// The parser probe: DOMParser, and the second document it brings with it.
//
// Parsing a string into a tree is the easy half and is barely worth a test.
// The hard half is that the result is a DIFFERENT DOCUMENT from the one on
// screen, and everything this file pins is about that boundary holding:
//
//   1. Queries are scoped. A parsed document asked for an id that exists only
//      in the live page answers null, and the live page asked for an id that
//      exists only in the parsed tree answers null. A lookup that fell through
//      in either direction would pass every "does it parse" test ever written.
//
//   2. Appending a parsed node into the live tree ADOPTS it — the live
//      document takes ownership, the parser document gives it up, and the id
//      map on each side moves with it. Without that the node renders correctly
//      and is destroyed by a document it no longer lives in.
//
//   3. Identity survives the move. The wrapper the app is holding is the same
//      value the live document hands back afterwards, because the registry is
//      keyed on the node and not on the document it happened to be in.
//
//   4. A MutationObserver works in a parsed document — AFTER one has already
//      been installed in the live one. Two documents, two hooks; the host used
//      to record only that some hook existed.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// Elements print as their id so a wrong element is a visibly wrong line rather
// than a matching `true`; a missing one prints as `null` rather than throwing.
function name(el) { return el ? (el.id || '<' + el.tagName + '>') : 'null'; }

const parser = new DOMParser();

// One line, no whitespace between tags: `children` counts elements, but
// `body.children.length` is only a stable number if nothing invisible is
// creating siblings.
const MARKUP =
    '<html><head><title>parsed</title></head><body>' +
    '<div id="card" class="a b" data-kind="widget"><span id="inner">hello</span></div>' +
    '<p id="tail">bye</p>' +
    '</body></html>';

const doc = parser.parseFromString(MARKUP, 'text/html');

// ---------------------------------------------------------------------------
// The tree that came back
// ---------------------------------------------------------------------------

say('doc.root', doc.documentElement.tagName);
say('doc.body', doc.body.tagName);
say('doc.bodyKids', doc.body.children.length);

const card = doc.getElementById('card');
say('card.found', name(card));
say('card.class', card.className);
say('card.attr', card.getAttribute('data-kind'));
say('card.text', card.textContent);
say('tail.text', doc.querySelector('#tail').textContent);

// Identity inside the parsed document is the same contract it is in the live
// one: two ways of reaching a node are one value.
say('card.identity', doc.querySelector('.a') === card);

// ---------------------------------------------------------------------------
// Claim 1: the two documents do not see each other
// ---------------------------------------------------------------------------

say('scope.liveIdInParsed', name(doc.getElementById('live_only')));
say('scope.parsedIdInLive', name(document.getElementById('card')));

// A second parse is a second document, not a second view of the first.
const doc2 = parser.parseFromString(MARKUP, 'text/html');
say('scope.twoParses', doc2.getElementById('card') !== card);

// A node created BY the parsed document lands in it, and only in it.
const made = doc.createElement('em');
made.id = 'made';
doc.body.appendChild(made);
say('made.inParsed', name(doc.querySelector('#made')));
say('made.inLive', name(document.querySelector('#made')));

// ---------------------------------------------------------------------------
// Claims 2 and 3: adoption
// ---------------------------------------------------------------------------

const host = document.getElementById('host');
host.appendChild(card);

say('adopt.parent', name(card.parentNode));
say('adopt.hostKids', host.children.length);
// The live document registered it...
say('adopt.inLive', name(document.getElementById('card')));
// ...the parser document let it go...
say('adopt.inParsed', name(doc.getElementById('card')));
// ...and it left that document's body behind. (`made` is still there, so this
// is 2 rather than 1: card out, em in.)
say('adopt.parsedKids', doc.body.children.length);
// The whole subtree moved, not just the node named — a descendant is
// registered in the live document too.
say('adopt.deep', document.getElementById('inner') === card.firstElementChild);
// Claim 3.
say('adopt.identity', document.getElementById('card') === card);

// ---------------------------------------------------------------------------
// A refusal
// ---------------------------------------------------------------------------

// Not a string. Left to itself this would parse the text "[object Object]"
// into an empty document and hand back something the app finds mysteriously
// blank much later.
let refused = 'none';
try {
    parser.parseFromString({}, 'text/html');
} catch (e) {
    refused = e.name;
}
say('refuse.object', refused);

// ---------------------------------------------------------------------------
// Claim 4: an observer in each document
// ---------------------------------------------------------------------------

const liveRecs = [];
const liveMo = new MutationObserver(function (records) {
    for (const r of records) liveRecs.push(r.type + '/' + r.attributeName);
});
// FIRST, and in the live document: this is what used to make the next one
// silent.
liveMo.observe(document.getElementById('pen'), { attributes: true });

const odoc = parser.parseFromString('<div id="watched"></div>', 'text/html');
const watched = odoc.getElementById('watched');
const parsedRecs = [];
const parsedMo = new MutationObserver(function (records) {
    for (const r of records) parsedRecs.push(r.type + '/' + r.attributeName);
});
parsedMo.observe(watched, { attributes: true });

// ---------------------------------------------------------------------------
// The frames
// ---------------------------------------------------------------------------

let frames = 0;
function tick() {
    frames++;
    if (frames === 1) {
        document.getElementById('pen').setAttribute('data-live', '1');
        watched.setAttribute('data-parsed', '1');
    }
    if (frames < 4) { requestAnimationFrame(tick); return; }

    say('obs.live', liveRecs.length ? liveRecs.join(',') : 'absent');
    say('obs.parsed', parsedRecs.length ? parsedRecs.join(',') : 'absent');
    // The attribute really is on the parsed node, whatever the observer said —
    // so a missing record is a delivery failure and not a failed setAttribute.
    say('obs.parsedAttr', watched.getAttribute('data-parsed'));

    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
}
requestAnimationFrame(tick);
