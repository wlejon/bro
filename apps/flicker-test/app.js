var valEl = document.getElementById('val');
var deltaEl = document.getElementById('delta');
var tickEl = document.getElementById('tick-count');
var value = 100;
var ticks = 0;

function update() {
    ticks++;
    tickEl.textContent = 'ticks: ' + ticks;

    var d = Math.floor(Math.random() * 21) - 10;
    value += d;
    valEl.textContent = String(value);

    if (d > 0) {
        deltaEl.textContent = '+' + d;
        deltaEl.className = 'up';
    } else if (d < 0) {
        deltaEl.textContent = String(d);
        deltaEl.className = 'down';
    } else {
        deltaEl.textContent = '--';
        deltaEl.className = 'flat';
    }
}

setInterval(update, 500);
