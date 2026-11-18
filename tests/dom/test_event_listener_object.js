// addEventListener takes an EventListener object as well as a function: its
// handleEvent, read at each dispatch, is called with the object as `this`
// (DOM "inner invoke"). SortableJS registers itself this way
// (on(document, 'pointermove', this)); bro threw "listener must be a
// function" and the drag never ended.

const root = document.getElementById('root');
root.innerHTML = '<div id="t"></div>';
const t = document.getElementById('t');

const seen = [];
const obj = { name: 'obj', handleEvent(e) { seen.push(this.name + ':' + e.type + ':' + (e.currentTarget === t)); } };
t.addEventListener('ping', obj);
t.addEventListener('ping', obj);  // a repeat is a no-op
t.dispatchEvent(new Event('ping'));
assert(seen.join() === 'obj:ping:true', 'handleEvent is called once with the object as this, got ' + seen.join());

// handleEvent is read at each dispatch.
obj.handleEvent = function (e) { seen.push('second:' + e.type); };
t.dispatchEvent(new Event('ping'));
assert(seen[1] === 'second:ping', 'a replaced handleEvent is the one called, got ' + seen[1]);

t.removeEventListener('ping', obj);
t.dispatchEvent(new Event('ping'));
assert(seen.length === 2, 'removeEventListener removes the object');

// document and window take one too.
let docHits = 0, winHits = 0;
const d = { handleEvent() { docHits++; } };
const w = { handleEvent() { winHits++; } };
document.addEventListener('pong', d);
window.addEventListener('pong', w);
t.dispatchEvent(new Event('pong', { bubbles: true }));
assert(docHits === 1 && winHits === 1, 'document and window objects are called, got ' + docHits + ' ' + winHits);
document.removeEventListener('pong', d);
window.removeEventListener('pong', w);

// A non-object is a TypeError; an object without handleEvent registers, and
// its dispatch reports a TypeError without stopping the next listener.
let threw = false;
try { t.addEventListener('x', 42); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'a number is refused with a TypeError');
let after = 0;
t.addEventListener('x', {});
t.addEventListener('x', () => after++);
t.dispatchEvent(new Event('x'));
assert(after === 1, 'the listener after a broken one still runs');
