// Fixture for test_script_rejection_fails.js: a module (it imports) whose
// native top-level await rejects.
import { value } from './module_dep.js';
await Promise.resolve(value);
await Promise.reject(new Error('fixture: module tla rejected'));
console.log('FIXTURE_UNREACHABLE');
