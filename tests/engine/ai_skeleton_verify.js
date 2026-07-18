// Driver evaluated inside the AI skeleton's own realm (spawned by
// test_ai_skeleton.js). Asserts the graceful no-model startup state without
// ever touching the Load-model button (native dialog — blocks in headless).

flush();

assert(document.querySelector('#load-btn'), 'load button present');

const st = document.querySelector('#status').textContent;
// Full builds show the pick-a-checkpoint message; AI-tower-less builds show
// the bro.lm-stub message. Both are the intended graceful state.
assert(st.indexOf('No model loaded') === 0 ||
       st.indexOf('This build has no language-model support') === 0,
       'graceful no-model status, got: ' + st);

assert(document.querySelector('#prompt').disabled === true,
       'composer input disabled without a model');
assert(document.querySelector('#send-btn').disabled === true,
       'send button disabled without a model');

const badge = document.querySelector('#gpu-badge').textContent;
assert(badge === 'CUDA' || badge === 'METAL' || badge === 'CPU',
       'bro.gpu probe rendered a backend badge, got: ' + badge);
assert(document.querySelector('#gpu-detail').textContent.indexOf('compiled:') !== -1,
       'probe detail lists compiledBackends');

console.log('AI_SKELETON_OK');
