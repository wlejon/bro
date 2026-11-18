// MutationObserver.observe(document, {childList, subtree}) sees the whole
// tree, as on the web (Chart.js watches document for its canvas's detach this
// way, and threw here because a Document was not a node to observe).
const seen = [];
const mo = new MutationObserver((records) => {
    for (const r of records) seen.push(r.type + ':' + [...r.addedNodes].map(n => n.nodeName).join('+') + '-' +
                                       [...r.removedNodes].map(n => n.nodeName).join('+'));
});
mo.observe(document, { childList: true, subtree: true });
const div = document.createElement('div');
const span = document.createElement('span');
div.appendChild(span);
document.body.appendChild(div);
div.remove();
assert(mo.takeRecords().length === 2, 'two childList records under body');
document.body.appendChild(div);
let delivered = false;
queueMicrotask(() => { delivered = true; });
flush();
assert(delivered && seen.length === 1 && seen[0] === 'childList:DIV-', 'delivered: ' + JSON.stringify(seen));
mo.disconnect();
div.remove();
flush();
assert(seen.length === 1, 'nothing after disconnect');

// Without subtree a Document registration sees no element's changes.
const shallow = new MutationObserver(() => { throw new Error('no records expected'); });
shallow.observe(document, { childList: true });
document.body.appendChild(document.createElement('p'));
assert(shallow.takeRecords().length === 0, 'no subtree, no records');
shallow.disconnect();
console.log('test_mutation_observe_document: OK');
