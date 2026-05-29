// Drives the real voice-pipeline app headless to verify it boots with no JS
// Worker: parallel async model loads on the main context -> talk button enabled.
//   bro-headless ../broworkshop/ai/voice-pipeline tests/_voice_app_boot.js
// Runs in the same (main) context as the app's main.js.

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(50); }
    return pred();
}

const talk = document.getElementById('talk');
assert(talk, 'talk button exists');
assert(talk.disabled, 'talk starts disabled (loading)');

const ok = pumpUntil(() => !talk.disabled, 180000);
assert(ok, 'models loaded and talk enabled within budget (no worker)');

console.log('[boot] talk: "' + talk.textContent + '"');
console.log('[boot] status: "' + document.getElementById('status').textContent + '"');
console.log('[boot] PASS');
