// Test createDocumentFragment, getElementsByTagName, getElementsByClassName,
// getElementsByName, and createComment.

// --- createDocumentFragment ---
var frag = document.createDocumentFragment();
assert(frag !== null, 'createDocumentFragment returns non-null');

// Add children to fragment
var a = document.createElement('div');
a.textContent = 'A';
var b = document.createElement('div');
b.textContent = 'B';
frag.appendChild(a);
frag.appendChild(b);

// Fragment children are accessible
assert(frag.childNodes.length === 2, 'fragment has 2 children');

// Appending fragment to DOM moves its children
var root = document.getElementById('root');
root.appendChild(frag);
flush();
assert(root.childNodes.length >= 2, 'root received fragment children');
assert(root.querySelector('div').textContent === 'A', 'first child is A');

// Fragment is now empty after appending
assert(frag.childNodes.length === 0, 'fragment empty after append');

// --- createComment ---
var comment = document.createComment('hello');
assert(comment !== null, 'createComment returns non-null');
assert(comment.nodeType === 8, 'comment nodeType is 8 (COMMENT_NODE)');
assert(comment.nodeName === '#comment', 'comment nodeName');
assert(comment.data === 'hello', 'comment data matches');

// Comment can be appended to DOM
root.appendChild(comment);

// --- getElementsByTagName ---
// Clear and set up fresh content
root.innerHTML = '<p>one</p><p>two</p><span>three</span>';
flush();

var paragraphs = document.getElementsByTagName('p');
assert(paragraphs.length === 2, 'getElementsByTagName("p") finds 2');

// Live collection: adding another <p> should be reflected
var p3 = document.createElement('p');
p3.textContent = 'four';
root.appendChild(p3);
flush();

// Re-query (live collections re-query on access)
var paragraphs2 = document.getElementsByTagName('p');
assert(paragraphs2.length === 3, 'getElementsByTagName is live — finds 3 after add');

// --- getElementsByClassName ---
root.innerHTML = '<div class="foo">a</div><div class="bar">b</div><div class="foo bar">c</div>';
flush();

var foos = document.getElementsByClassName('foo');
assert(foos.length === 2, 'getElementsByClassName("foo") finds 2');

var bars = document.getElementsByClassName('bar');
assert(bars.length === 2, 'getElementsByClassName("bar") finds 2');

// --- getElementsByName ---
root.innerHTML = '<input name="email"><input name="email"><input name="phone">';
flush();

var emails = document.getElementsByName('email');
assert(emails.length === 2, 'getElementsByName("email") finds 2');

var phones = document.getElementsByName('phone');
assert(phones.length === 1, 'getElementsByName("phone") finds 1');

// --- Cleanup ---
root.innerHTML = '';
