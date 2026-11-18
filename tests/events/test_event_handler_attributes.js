// The event handler attributes of the window and the document
// (host_event_handlers.cpp): window.onresize, window.onkeydown,
// document.onkeydown fire with the real events, read back what was set,
// a function set over a function takes the first one's place among the
// target's listeners, and null removes the handler.

const order = [];
const wFirst = () => order.push('window listener before');
window.addEventListener('keydown', wFirst);
window.onkeydown = () => order.push('window.onkeydown A');
window.addEventListener('keydown', () => order.push('window listener after'));
// Replaced in place: B runs where A was registered.
const handlerB = function (e) { order.push('window.onkeydown B ' + e.type + ' this=' + (this === window)); };
window.onkeydown = handlerB;
document.onkeydown = (e) => order.push('document.onkeydown ' + e.key);

assert(window.onkeydown === handlerB, 'window.onkeydown reads back what was set');
assert(document.onclick === null && window.onresize === null, 'an unset handler reads null');

keyDown('a');
keyUp('a');
assert(JSON.stringify(order) === JSON.stringify([
    'document.onkeydown a',
    'window listener before',
    'window.onkeydown B keydown this=true',
    'window listener after',
]), 'keydown reached the document handler, then the window\'s in registration order, got ' + JSON.stringify(order));

let resized = 0;
window.onresize = () => { resized++; };
resize(640, 360);
flush();
assert(resized === 1, 'window.onresize fired on resize, got ' + resized);

window.onresize = null;
resize(800, 600);
flush();
assert(resized === 1 && window.onresize === null, 'null removed the handler');

window.onkeydown = null;
document.onkeydown = 'not a function';
assert(document.onkeydown === null, 'a non-function value clears the handler');
order.length = 0;
keyDown('b');
keyUp('b');
assert(JSON.stringify(order) === JSON.stringify(['window listener before', 'window listener after']),
       'cleared handlers do not fire, got ' + JSON.stringify(order));

// A null or undefined listener is no registration, not an error (DOM
// addEventListener step 2); A-Frame passes an unset variable.
window.addEventListener('load', undefined);
window.addEventListener('load', null);
document.body.addEventListener('click', undefined);

console.log('test_event_handler_attributes: OK');
