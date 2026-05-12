// Test MutationObserver — exercises both the JS polyfill and the native
// notifyMutationObservers C++ pump (src/js/mutation_observer.cpp).

const root = document.getElementById('root');
root.innerHTML = '<div id="host"><span id="child">x</span></div>';
flush();
const host = document.getElementById('host');
const child = document.getElementById('child');

// --- Basic constructor validation ---
let threw = false;
try { new MutationObserver(); } catch (e) { threw = true; }
assert(threw, 'MutationObserver throws without callback');

threw = false;
try {
    const obs = new MutationObserver(() => {});
    obs.observe(host, {}); // empty options - should throw
} catch (e) { threw = true; }
assert(threw, 'observe throws when no observation type given');

// --- childList observation ---
let records1 = null;
const obs1 = new MutationObserver((r) => { records1 = r; });
obs1.observe(host, { childList: true });

const newChild = document.createElement('p');
newChild.textContent = 'added';
host.appendChild(newChild);

// Microtask delivers the record
await Promise.resolve();
assert(records1 !== null, 'childList records delivered');
assert(records1.length >= 1, 'at least one record');
assert(records1[0].type === 'childList', 'record.type = childList');
assert(records1[0].target === host || records1[0].target.id === 'host', 'target is host');
assert(records1[0].addedNodes.length >= 1, 'addedNodes populated');

// removeChild produces removedNodes record
records1 = null;
host.removeChild(newChild);
await Promise.resolve();
assert(records1 !== null && records1.length >= 1, 'remove records delivered');
const rec = records1[0];
assert(rec.removedNodes.length >= 1, 'removedNodes populated');

// --- attributes observation ---
let attrRecords = null;
const obs2 = new MutationObserver((r) => { attrRecords = r; });
obs2.observe(child, { attributes: true, attributeOldValue: true });

child.setAttribute('data-x', 'one');
await Promise.resolve();
assert(attrRecords !== null && attrRecords.length >= 1, 'attribute records');
const ar = attrRecords[0];
assert(ar.type === 'attributes', 'type = attributes');
assert(ar.attributeName === 'data-x', 'attributeName = data-x');

attrRecords = null;
child.setAttribute('data-x', 'two');
await Promise.resolve();
assert(attrRecords[0].oldValue === 'one', 'oldValue tracked, got: ' + attrRecords[0].oldValue);

// --- attributeFilter ---
let filtered = [];
const obs3 = new MutationObserver((r) => { filtered = filtered.concat(r); });
obs3.observe(child, { attributeFilter: ['data-keep'] });

child.setAttribute('data-keep', '1');
child.setAttribute('data-ignore', '2');
await Promise.resolve();
const keepRecs = filtered.filter(r => r.attributeName === 'data-keep');
const ignoreRecs = filtered.filter(r => r.attributeName === 'data-ignore');
assert(keepRecs.length >= 1, 'attributeFilter passes matching attr');
assert(ignoreRecs.length === 0, 'attributeFilter blocks non-matching attr');

// --- subtree observation ---
let subRecords = null;
const obs4 = new MutationObserver((r) => { subRecords = r; });
obs4.observe(host, { childList: true, subtree: true });

const deepChild = document.createElement('em');
child.appendChild(deepChild);
await Promise.resolve();
assert(subRecords !== null && subRecords.length >= 1, 'subtree records delivered');

// --- takeRecords ---
const obs5 = new MutationObserver(() => {});
obs5.observe(host, { childList: true });
host.appendChild(document.createElement('br'));
// Don't await - records are buffered
const taken = obs5.takeRecords();
// After takeRecords, the queued microtask should see no records
assert(Array.isArray(taken), 'takeRecords returns array');

// --- unobserve ---
const obs6 = new MutationObserver((r) => { throw new Error('should not fire'); });
obs6.observe(child, { attributes: true });
obs6.unobserve(child);
child.setAttribute('data-y', 'z');
await Promise.resolve();
// (no throw means unobserve worked)

// --- disconnect ---
obs1.disconnect();
obs2.disconnect();
obs3.disconnect();
obs4.disconnect();

// After disconnect, no more deliveries
records1 = null;
host.appendChild(document.createElement('span'));
await Promise.resolve();
assert(records1 === null, 'disconnect stops deliveries');

// --- characterData observation ---
const textNode = document.createTextNode('hello');
host.appendChild(textNode);
let textRec = null;
const obs7 = new MutationObserver((r) => { textRec = r; });
obs7.observe(textNode, { characterData: true, characterDataOldValue: true });
textNode.data = 'world';
await Promise.resolve();
assert(textRec !== null && textRec.length >= 1, 'characterData record');
assert(textRec[0].type === 'characterData', 'type = characterData');
assert(textRec[0].oldValue === 'hello', 'characterData oldValue, got: ' + textRec[0].oldValue);

obs7.disconnect();
root.innerHTML = '';
