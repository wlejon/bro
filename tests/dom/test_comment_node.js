// Test DOM CommentNode and TextNode CharacterData methods.
// Exercises src/dom/comment_node.cpp and the shared CharacterData
// bindings in src/js/node_bindings.cpp.

const root = document.getElementById('root');

// =========================================================================
// createComment
// =========================================================================
const c = document.createComment('hello world');
assert(c !== null, 'createComment returns a node');
assert(c.nodeType === 8, 'comment nodeType is 8');
assert(c.nodeName === '#comment', 'comment nodeName is #comment');
assert(c.data === 'hello world', 'comment data');
assert(c.length === 11, 'comment length');

// data setter
c.data = 'replaced';
assert(c.data === 'replaced', 'data setter works');
assert(c.length === 8, 'length tracks data');

// substringData
assert(c.substringData(0, 4) === 'repl', 'substringData basic');
assert(c.substringData(4, 4) === 'aced', 'substringData mid');
assert(c.substringData(0, 100) === 'replaced', 'substringData past end returns rest');
// past end returns empty (per impl)
assert(c.substringData(100, 1) === '', 'substringData past length returns empty');

// appendData
c.data = 'foo';
c.appendData('bar');
assert(c.data === 'foobar', 'appendData');

// insertData
c.insertData(3, 'BAZ');
assert(c.data === 'fooBAZbar', 'insertData mid');
c.insertData(100, 'X'); // past length clamps to end
assert(c.data === 'fooBAZbarX', 'insertData past length appends');

// deleteData
c.data = 'abcdefgh';
c.deleteData(2, 3);
assert(c.data === 'abfgh', 'deleteData');
c.data = 'short';
c.deleteData(100, 2); // past length: no-op
assert(c.data === 'short', 'deleteData past length is no-op');

// replaceData
c.data = 'hello';
c.replaceData(0, 5, 'world');
assert(c.data === 'world', 'replaceData full');
c.data = 'abcdef';
c.replaceData(1, 3, 'XX');
assert(c.data === 'aXXef', 'replaceData partial shrink');

// Insertion into DOM
const wrapper = document.createElement('div');
wrapper.appendChild(c);
assert(c.parentNode === wrapper, 'comment parentNode after append');
root.appendChild(wrapper);
flush();

// =========================================================================
// TextNode CharacterData (shares same bindings)
// =========================================================================
const t = document.createTextNode('Hello, world');
assert(t.nodeType === 3, 'text nodeType is 3');
assert(t.data === 'Hello, world', 'text data');
assert(t.length === 12, 'text length');

t.appendData('!');
assert(t.data === 'Hello, world!', 'text appendData');

t.insertData(5, ' there');
assert(t.data === 'Hello there, world!', 'text insertData');

t.deleteData(5, 6); // remove ' there'
assert(t.data === 'Hello, world!', 'text deleteData');

t.replaceData(7, 5, 'earth');
assert(t.data === 'Hello, earth!', 'text replaceData');

assert(t.substringData(7, 5) === 'earth', 'text substringData');

// Cleanup
root.innerHTML = '';
