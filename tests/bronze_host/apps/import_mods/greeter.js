// An INTERPRETED module: never compiled, reached only through a compiled
// `import()` whose specifier bronze could not read at build time
// (import_probe.js). It lives beside the probe's source because that is
// what the importer's `import.meta.url` names.
import { suffix } from './suffix.js';

export const greeting = 'hello';
export function greet(name) { return 'hi ' + name + suffix; }
export default 42;
