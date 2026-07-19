// A text node is a node: parentElement and contains() must see it.
//
// Both used to unwrap their argument as an Element, which yields null for a
// Text node — so `div.contains(div.firstChild)` answered false for a child the
// element demonstrably owns, and `textNode.parentElement` was null for a
// parent `textNode.parentNode` reported fine one line earlier.
//
// The consequence was not cosmetic. Every guard written as "is this node
// inside my subtree" silently excluded all text, which is most of a document.

const root = document.getElementById('root');

// --- parentElement on a text node -------------------------------------------
{
    root.innerHTML = '<div id="a">hello</div>';
    flush();
    const a = document.getElementById('a');
    const t = a.firstChild;

    assert(t.nodeType === 3, 'the child is a text node, got nodeType ' + t.nodeType);
    // parentNode has always worked; parentElement is the narrowed form and
    // must agree with it whenever the parent is an element.
    assert(t.parentNode === a, 'parentNode is the div');
    assert(t.parentElement === a,
           'and parentElement is the same div, not null');
    assert(t.parentElement.tagName === 'DIV',
           'so .parentElement.tagName reads without throwing, got ' +
           t.parentElement.tagName);
}

// --- parentElement is null when the parent is not an element -----------------
//
// The contrast that makes the above meaningful: parentElement is not simply
// parentNode renamed. Inside a DocumentFragment a node has a parentNode and no
// parentElement. (bro models a fragment as an Element with a reserved tag, so
// this also pins that the shim does not leak through as a real parent.)
{
    const frag = document.createDocumentFragment();
    const t = document.createTextNode('x');
    frag.appendChild(t);

    assert(t.parentNode !== null, 'a node in a fragment has a parentNode');
    assert(t.parentNode.nodeType === 11, 'and it is the fragment, nodeType 11');
    assert(t.parentElement === null,
           'but parentElement is null — a DocumentFragment is not an Element');

    // Detached, with no parent at all.
    const loose = document.createTextNode('y');
    assert(loose.parentNode === null && loose.parentElement === null,
           'a detached node has neither');
}

// --- contains() accepts a text node ------------------------------------------
{
    root.innerHTML = '<div id="b">one<span id="c">two</span>three</div>';
    flush();
    const b = document.getElementById('b');
    const c = document.getElementById('c');
    const first = b.firstChild;          // "one"
    const inner = c.firstChild;          // "two", one level deeper
    const last = b.lastChild;            // "three"

    assert(b.contains(first), 'a div contains its own first text child');
    assert(b.contains(last), 'and its last text child');
    assert(b.contains(inner),
           'and a text node nested inside a child element — contains() is the ' +
           'whole subtree, not just direct children');
    assert(c.contains(inner), 'the span contains its own text');
    assert(!c.contains(first),
           'but not its sibling\'s text — otherwise the walk is not checking ' +
           'ancestry at all');
    // Per spec contains() is inclusive of the node itself.
    assert(b.contains(b), 'contains() includes the node itself');
    assert(document.body.contains(first),
           'and it works at any depth, from the body down to a text node');
}

// --- compareDocumentPosition sees text nodes too -----------------------------
//
// Same unwrap, same defect: a Text argument made it return 0, which reads as
// "these are the same node" rather than "contained by".
{
    root.innerHTML = '<div id="d">text</div>';
    flush();
    const d = document.getElementById('d');
    const t = d.firstChild;
    const pos = d.compareDocumentPosition(t);
    assert(pos !== 0,
           'a div and its text child are not the same node, got ' + pos);
    // 16 = CONTAINED_BY, 4 = FOLLOWING. Both set for a descendant.
    assert((pos & 16) !== 0,
           'the text node is CONTAINED_BY the div, got ' + pos);
}

root.innerHTML = '';
