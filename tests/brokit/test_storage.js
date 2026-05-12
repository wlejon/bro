// Test localStorage / sessionStorage.

assert(typeof localStorage === 'object', 'localStorage exists');

// Clear first
localStorage.clear();
assert(localStorage.length === 0, 'cleared length 0: ' + localStorage.length);

localStorage.setItem('a', '1');
localStorage.setItem('b', 'two');
assert(localStorage.length === 2, 'length 2 after sets: ' + localStorage.length);
assert(localStorage.getItem('a') === '1', 'get a');
assert(localStorage.getItem('b') === 'two', 'get b');
assert(localStorage.getItem('missing') === null, 'missing is null');

// Coercion: non-strings should be stringified
localStorage.setItem('num', 42);
assert(localStorage.getItem('num') === '42', 'num coerced to string: ' + localStorage.getItem('num'));

// key(index)
const keys = new Set();
for (let i = 0; i < localStorage.length; i++) keys.add(localStorage.key(i));
assert(keys.has('a') && keys.has('b'), 'keys iterable');

// removeItem
localStorage.removeItem('a');
assert(localStorage.getItem('a') === null, 'removed a');
assert(localStorage.length === 2, 'length after remove: ' + localStorage.length); // b + num

// clear
localStorage.clear();
assert(localStorage.length === 0, 'cleared again');

// sessionStorage
assert(typeof sessionStorage === 'object', 'sessionStorage exists');
sessionStorage.clear();
sessionStorage.setItem('x', 'y');
assert(sessionStorage.getItem('x') === 'y', 'session get');
assert(sessionStorage.length === 1, 'session length');
sessionStorage.clear();

// Independence
localStorage.setItem('shared', 'L');
sessionStorage.setItem('shared', 'S');
assert(localStorage.getItem('shared') === 'L', 'local vs session L');
assert(sessionStorage.getItem('shared') === 'S', 'local vs session S');
localStorage.clear();
sessionStorage.clear();
