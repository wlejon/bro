// Fixture for test_script_rejection_fails.js: the rejection comes after the
// script's top level has returned — a timer later.
await new Promise((resolve) => setTimeout(resolve, 100));
console.log('FIXTURE_RESUMED');
throw new Error('fixture: failed after the timer');
