// Fixture for test_script_rejection_fails.js: a rejection nothing handles,
// from a script whose own top level completes normally.
Promise.reject(new Error('fixture: nobody handled this'));
console.log('FIXTURE_TOP_LEVEL_DONE');
