// Verify HTMLMediaElement IDL props actually affect behavior.
var vid = document.getElementById("v");

function header(t) { console.log('\n== ' + t + ' =='); }

// Wait for metadata by pumping frames.
function waitMetadata() {
  for (let i = 0; i < 20; i++) {
    if (vid.readyState >= 1 && isFinite(vid.duration) && vid.duration > 0) return;
    sleep(100);
    flush();
    screenshot('/tmp/_vid_wait.png');
  }
}
waitMetadata();
console.log('metadata ready: dur=' + vid.duration.toFixed(3) + ' size=' + vid.videoWidth + 'x' + vid.videoHeight);

// -------- playbackRate --------
header('playbackRate');
console.log('default rate=' + vid.playbackRate);
vid.playbackRate = 2.0;
console.log('set 2.0 -> rate=' + vid.playbackRate);

// Measure how much media time advances per 500ms of wall time at rate=2.0
vid.currentTime = 0;
flush(); screenshot('/tmp/_vid.png');
vid.play();
const t0 = vid.currentTime;
const wall0 = Date.now();
sleep(500); flush(); screenshot('/tmp/_vid.png');
const t1 = vid.currentTime;
const wall1 = Date.now();
vid.pause();
console.log('rate=2.0: media advanced ' + (t1 - t0).toFixed(3) + 's over wall ' + (wall1 - wall0) + 'ms');
console.log('  (at rate=2.0, expect media ~2x wall; at rate=1.0, expect ~1x)');

// Reset and try rate=0.5
vid.currentTime = 0;
vid.playbackRate = 0.5;
flush(); screenshot('/tmp/_vid.png');
vid.play();
const t2 = vid.currentTime;
sleep(500); flush(); screenshot('/tmp/_vid.png');
const t3 = vid.currentTime;
vid.pause();
console.log('rate=0.5: media advanced ' + (t3 - t2).toFixed(3) + 's over wall 500ms');

// -------- defaultMuted --------
header('defaultMuted');
console.log('initial muted=' + vid.muted + ' defaultMuted=' + vid.defaultMuted + ' hasAttr(muted)=' + vid.hasAttribute('muted'));
vid.defaultMuted = true;
console.log('after defaultMuted=true: muted=' + vid.muted + ' defaultMuted=' + vid.defaultMuted + ' hasAttr(muted)=' + vid.hasAttribute('muted'));
// Spec says: defaultMuted reflects the "muted" content attribute. It only
// influences the "muted" IDL getter at element creation; setting it later
// should NOT flip current muted state. BUT the current load pathway in
// ElVideo doesn't read the attribute at load, so defaultMuted is effectively
// a no-op across reloads too.
vid.load();
waitMetadata();
console.log('after load() with defaultMuted=true: muted=' + vid.muted);

// -------- controls --------
header('controls');
console.log('initial controls=' + vid.controls + ' hasAttr=' + vid.hasAttribute('controls'));
vid.controls = true;
console.log('after controls=true: controls=' + vid.controls + ' hasAttr=' + vid.hasAttribute('controls'));
// We have no controls UI. Take a screenshot so we can see whether anything renders.
vid.currentTime = 0.5;
flush();
screenshot('/tmp/_vid_controls.png');
console.log('screenshot saved: /tmp/_vid_controls.png (expect no overlay since no controls UI)');
