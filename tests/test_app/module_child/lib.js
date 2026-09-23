// Imported by both module scripts of module_child/index.html. The realm's
// module registry evaluates it once, so both see one `counter`.
export const counter = { n: 0 };
counter.n++;
export function greet(who) {
  return 'hello ' + who;
}
