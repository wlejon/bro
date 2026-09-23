// Fixture for test_script_rejection_fails.js: a script that awaits a timer and
// a module import, rejects a promise and cancels its report — and passes.
import { value } from './module_dep.js';
window.addEventListener('unhandledrejection', (e) => e.preventDefault());
Promise.reject(new Error('fixture: cancelled report'));
await new Promise((resolve) => setTimeout(resolve, 100));
console.log('FIXTURE_RESUMED ' + value);
