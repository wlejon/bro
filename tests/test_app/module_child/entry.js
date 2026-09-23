// An external module script of module_child/index.html.
import { counter } from './lib.js';
window.__order.push('external');
window.__counterInExternal = counter.n;
// Top-level await in a module script.
await Promise.resolve();
window.__afterAwait = true;
