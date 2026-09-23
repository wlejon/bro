// <script type=module> inside an <iframe> sub-document: inline and external
// module scripts run, in document order with the classic scripts, with
// imports resolved against the frame's document, module scope, one instance
// of a module both scripts import, and top-level await.

const el = document.createElement('iframe');
el.setAttribute('src', 'module_child');
el.style.width = '200px';
el.style.height = '120px';
document.body.appendChild(el);
flush();
advanceTime(16);

const cw = el.contentWindow;
assert(cw !== null, 'frame loaded');
const order = cw.__order;
assert(Array.isArray(order) && order.join(',') === 'classic,inline,external',
    'classic and module scripts ran in document order, got ' + (order && order.join(',')));
assert(cw.__greeting === 'hello frame', 'the inline module imported ./lib.js, got ' + cw.__greeting);
assert(cw.__hasModuleScopeLeak === false, 'a module\'s top-level const stays out of the global scope');
assert(cw.__counterAfterInline === 1 && cw.__counterInExternal === 1,
    'both modules share one evaluation of lib.js, got ' + cw.__counterAfterInline + '/' + cw.__counterInExternal);
assert(cw.__afterAwait === true, 'top-level await in an external module script completed');
assert(cw.document.getElementById('out').textContent === 'hello frame', 'the module wrote into the frame\'s document');
assert(typeof window.__order === 'undefined', 'the frame\'s globals did not land in the host realm');

el.remove();
flush();
console.log('test_iframe_module_scripts: OK');
