// Headless smoke test for <video> media events.
const vid = document.getElementById('v');
const log = [];
vid.addEventListener('loadedmetadata', () => log.push('loadedmetadata ' + vid.videoWidth + 'x' + vid.videoHeight));
vid.addEventListener('timeupdate', () => log.push('timeupdate ' + vid.currentTime.toFixed(2)));
vid.addEventListener('ended', () => log.push('ended ' + vid.currentTime.toFixed(2)));

vid.play();
// FileClock ticks off steady_clock, so we sleep real wall-clock.
// screenshot() forces a full draw traversal which invokes ElVideo::draw()
// and pumps media events; flush alone does not paint.
for (let i = 0; i < 12; i++) {
  sleep(250);
  flush();
  screenshot('/tmp/_video_tick.png');
}
console.log('EVENTS:');
for (const line of log) console.log('  ' + line);
console.log('final t=' + vid.currentTime.toFixed(3) + ' dur=' + vid.duration.toFixed(3) + ' paused=' + vid.paused);
