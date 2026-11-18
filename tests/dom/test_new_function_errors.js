// A body that does not compile makes `new Function` throw a catchable
// SyntaxError and harms nothing: later `new Function` calls in the page still
// work (the first failure used to latch the page's failure flag, and every
// later one failed with "dynamic compilation failed").
let threw = null;
try { new Function('return )'); } catch (e) { threw = e; }
assert(threw instanceof SyntaxError, 'a bad body is a SyntaxError: ' + threw);
const add = new Function('x', 'y', 'return x + y');
assert(add(2, 3) === 5, 'later new Function works');
assert(Function('return 7')() === 7, 'Function() without new');
assert(add.name === 'anonymous', 'named anonymous: ' + add.name);
const gen = Object.getPrototypeOf(function* () {}).constructor;
assert([...gen('yield 1; yield 2')()].join() === '1,2', 'GeneratorFunction');
let runtime = null;
try { new Function('throw new RangeError("r")')(); } catch (e) { runtime = e; }
assert(runtime instanceof RangeError, 'a throw from the body reaches the caller');
console.log('test_new_function_errors: OK');
