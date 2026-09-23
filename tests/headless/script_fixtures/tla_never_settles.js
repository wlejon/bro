// Fixture for test_script_rejection_fails.js: a top-level await that never
// settles. The rest of the script never runs, so the run has not passed.
await new Promise(() => {});
console.log('FIXTURE_UNREACHABLE');
