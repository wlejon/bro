const v = document.getElementById('v');
const status = document.getElementById('status');
const eventsEl = document.getElementById('events');
const progbar = document.getElementById('progbar');

// ---- event log -------------------------------------------------------------

const eventLog = [];
function logEvent(name, extra) {
  const line = name + (extra ? ' ' + extra : '');
  eventLog.push(line);
  if (eventLog.length > 30) eventLog.shift();
  eventsEl.textContent = eventLog.join('\n');
  eventsEl.scrollTop = 1e9;
}

const mediaEvents = [
  'loadedmetadata', 'loadeddata', 'canplay', 'canplaythrough',
  'play', 'pause', 'ended',
  'seeking', 'seeked', 'timeupdate',
  'volumechange', 'ratechange', 'durationchange',
  'waiting', 'stalled', 'error',
];
for (const name of mediaEvents) {
  v.addEventListener(name, (e) => {
    if (name === 'timeupdate') return; // too noisy — shown in status
    logEvent(name);
  });
}

// ---- playback buttons ------------------------------------------------------

document.getElementById('play').onclick  = () => v.play();
document.getElementById('pause').onclick = () => v.pause();
document.getElementById('load').onclick  = () => v.load();
document.getElementById('seek0').onclick = () => { v.currentTime = 0; };
document.getElementById('seek1').onclick = () => { v.currentTime = 1.0; };
document.getElementById('seek-end').onclick = () => {
  v.currentTime = Math.max(0, (v.duration || 0) - 0.2);
};

// ---- src -------------------------------------------------------------------

document.getElementById('apply-src').onclick = () => {
  v.src = document.getElementById('src-input').value;
};

// ---- volume / rate ---------------------------------------------------------

const volumeInput = document.getElementById('volume');
const volumeVal = document.getElementById('volume-val');
volumeInput.oninput = () => {
  v.volume = parseFloat(volumeInput.value);
  volumeVal.textContent = v.volume.toFixed(2);
};

const mutedInput = document.getElementById('muted');
mutedInput.onchange = () => { v.muted = mutedInput.checked; };

const rateInput = document.getElementById('rate');
const rateVal = document.getElementById('rate-val');
rateInput.oninput = () => {
  v.playbackRate = parseFloat(rateInput.value);
  rateVal.textContent = v.playbackRate.toFixed(2);
};

// ---- attribute flags -------------------------------------------------------

const bindFlag = (id, prop) => {
  const el = document.getElementById(id);
  el.onchange = () => { v[prop] = el.checked; };
};
bindFlag('autoplay', 'autoplay');
bindFlag('loop', 'loop');
bindFlag('controls', 'controls');
bindFlag('defaultMuted', 'defaultMuted');

document.getElementById('preload').onchange = (e) => {
  v.preload = e.target.value;
};

// ---- canPlayType -----------------------------------------------------------

const cptResult = document.getElementById('cpt-result');
const cpt = (mime) => () => {
  const r = v.canPlayType(mime);
  cptResult.textContent = mime + ' => "' + r + '"';
};
document.getElementById('cpt-webm').onclick = cpt('video/webm');
document.getElementById('cpt-vp9').onclick  = cpt('video/webm; codecs="vp9,opus"');
document.getElementById('cpt-mp4').onclick  = cpt('video/mp4');

// ---- clear events ----------------------------------------------------------

document.getElementById('clear-events').onclick = () => {
  eventLog.length = 0;
  eventsEl.textContent = '';
};

// ---- status tick -----------------------------------------------------------

const fmt = (x) => Number.isFinite(x) ? x.toFixed(3) : String(x);
const tr = (ranges) => {
  if (!ranges || ranges.length === 0) return '(empty)';
  const parts = [];
  for (let i = 0; i < ranges.length; i++) {
    parts.push('[' + fmt(ranges.start(i)) + ',' + fmt(ranges.end(i)) + ']');
  }
  return parts.join(' ');
};

function tick() {
  const dur = v.duration;
  const t = v.currentTime;
  const pct = (Number.isFinite(dur) && dur > 0) ? (t / dur * 100) : 0;
  progbar.style.width = pct.toFixed(1) + '%';

  status.textContent =
    'src:          ' + v.src + '\n' +
    'currentSrc:   ' + v.currentSrc + '\n' +
    'paused:       ' + v.paused + '    ended: ' + v.ended + '    seeking: ' + v.seeking + '\n' +
    'currentTime:  ' + fmt(t) + ' / ' + fmt(dur) + '\n' +
    'volume:       ' + fmt(v.volume) + '    muted: ' + v.muted + '    defaultMuted: ' + v.defaultMuted + '\n' +
    'playbackRate: ' + fmt(v.playbackRate) + '    default: ' + fmt(v.defaultPlaybackRate) + '\n' +
    'readyState:   ' + v.readyState + '    networkState: ' + v.networkState + '\n' +
    'video size:   ' + v.videoWidth + 'x' + v.videoHeight + '\n' +
    'autoplay=' + v.autoplay + '  loop=' + v.loop + '  controls=' + v.controls + '  preload=' + v.preload + '\n' +
    'buffered:     ' + tr(v.buffered) + '\n' +
    'seekable:     ' + tr(v.seekable) + '\n' +
    'played:       ' + tr(v.played);

  requestAnimationFrame(tick);
}
tick();
