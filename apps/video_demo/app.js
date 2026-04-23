const v = document.getElementById('v');
const status = document.getElementById('status');

document.getElementById('play').onclick  = () => v.play();
document.getElementById('pause').onclick = () => v.pause();
document.getElementById('seek').onclick  = () => { v.currentTime = 1.0; };

function tick() {
  status.textContent =
    'paused=' + v.paused +
    '  t=' + v.currentTime.toFixed(3) +
    '/' + v.duration.toFixed(3) +
    '  ' + v.videoWidth + 'x' + v.videoHeight;
  requestAnimationFrame(tick);
}
tick();
