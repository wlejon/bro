// Test <iframe> contentWindow property and realm proxy.

const CHILD = 'iframe_child';

const el = document.createElement('iframe');
assert(el.contentWindow === null, 'contentWindow is null before attachment');

el.setAttribute('src', CHILD);
el.style.width = '200px';
el.style.height = '120px';
document.body.appendChild(el);
flush();

const cw = el.contentWindow;
assert(cw !== null, 'contentWindow is non-null after load');
assert(typeof cw === 'object', 'contentWindow is an object');
assert(cw.frameElement === el, 'contentWindow.frameElement is iframe element');
assert(cw.window === cw, 'contentWindow.window is self-referential');
assert(cw.self === cw, 'contentWindow.self is self-referential');
assert(cw.parent === window, 'contentWindow.parent is parent window');
assert(cw.top === window, 'contentWindow.top is top window');
assert(cw.document !== null, 'contentWindow.document is accessible');
assert(cw.document !== document, 'contentWindow.document is not host document');

// Check that child document can be queried
assert(typeof cw.document.title === 'string', 'child document has title property');

// Check that properties can be set and read on contentWindow
cw.testProp = 'hello_from_parent';
assert(cw.testProp === 'hello_from_parent', 'can set and read property on contentWindow');

console.log('test_iframe_contentwindow OK');
