// === Easing ===
function easeOutCubic(t) {
    return 1 - Math.pow(1 - t, 3);
}

// === Animation engine ===
var animations = [];
var animTime = 0;

function animate(element, prop, from, to, startMs, durationMs, unit) {
    animations.push({
        el: element, prop: prop, from: from, to: to,
        start: startMs, dur: durationMs, unit: unit || "",
        done: false
    });
}

function tickAnimations(dt) {
    animTime += dt;
    for (var i = 0; i < animations.length; i++) {
        var a = animations[i];
        if (a.done) continue;
        if (animTime < a.start) continue;
        var elapsed = animTime - a.start;
        var t = Math.min(elapsed / a.dur, 1);
        var val = a.from + (a.to - a.from) * easeOutCubic(t);
        a.el.style[a.prop] = Math.round(val) + a.unit;
        if (t >= 1) a.done = true;
    }
}

// === Entrance sequence ===
var topbar = document.getElementById("topbar");
animate(topbar, "marginTop", -60, 0, 100, 600, "px");

var sidebar = document.getElementById("sidebar");
animate(sidebar, "marginLeft", -200, 0, 200, 700, "px");

for (var c = 0; c < 4; c++) {
    var card = document.getElementById("card" + c);
    animate(card, "marginLeft", 400, 0, 400 + c * 120, 600, "px");
}

var chartPanel = document.getElementById("chart-panel");
var feedPanel = document.getElementById("feed-panel");
animate(chartPanel, "marginTop", 100, 0, 900, 600, "px");
animate(feedPanel, "marginTop", 100, 0, 1050, 600, "px");

for (var g = 0; g < 6; g++) {
    var gauge = document.getElementById("gauge" + g);
    animate(gauge, "marginTop", 100, 0, 1200 + g * 80, 500, "px");
}

// === Live data ===
var cpuVal = 45, memVal = 62, netVal = 30, diskVal = 25;
var latencyVal = 12, errorVal = 0.3;
var connsVal = 1420, queueVal = 23, cacheVal = 94;
var chartBars = [65, 72, 58, 80, 91, 85, 78, 95, 88, 70, 82, 90];
var chartTargets = chartBars.slice();

function randomWalk(val, min, max, step) {
    val += (Math.random() - 0.5) * step;
    if (val < min) val = min;
    if (val > max) val = max;
    return val;
}

function lerp(a, b, t) { return a + (b - a) * t; }

// Cache element references
var statCpu = document.getElementById("stat-cpu");
var statMem = document.getElementById("stat-mem");
var statNet = document.getElementById("stat-net");
var statDisk = document.getElementById("stat-disk");
var barCpu = document.getElementById("bar-cpu");
var barMem = document.getElementById("bar-mem");
var barNet = document.getElementById("bar-net");
var barDisk = document.getElementById("bar-disk");
var gLatency = document.getElementById("gauge-latency");
var gErrors = document.getElementById("gauge-errors");
var gUptime = document.getElementById("gauge-uptime");
var gConns = document.getElementById("gauge-conns");
var gQueue = document.getElementById("gauge-queue");
var gCache = document.getElementById("gauge-cache");
var gbLatency = document.getElementById("gbar-latency");
var gbErrors = document.getElementById("gbar-errors");
var gbUptime = document.getElementById("gbar-uptime");
var gbConns = document.getElementById("gbar-conns");
var gbQueue = document.getElementById("gbar-queue");
var gbCache = document.getElementById("gbar-cache");
var clockEl = document.getElementById("clock");

var cbarEls = [];
for (var b = 0; b < 12; b++) {
    cbarEls.push(document.getElementById("cbar" + b));
}

// Feed: use 8 pre-existing slots, rotate text content
var feedSlots = [];
for (var f = 0; f < 8; f++) {
    feedSlots.push(document.getElementById("feed-" + f));
}

var feedMessages = [
    "Request processed in 12ms",
    "Cache miss on /api/users",
    "New connection from 10.0.1.42",
    "Query optimized: -34% latency",
    "Rate limit warning: 450/500",
    "SSL cert renewal in 14 days",
    "Timeout on upstream /auth",
    "Failover to replica-2 complete",
    "Disk I/O spike on /dev/sda1",
    "Deploy v2.14.1 rolling out",
    "Health check passed: all nodes",
    "Connection pool exhausted",
    "GC pause: 18ms",
    "Backup completed successfully",
    "DNS resolution slow: 340ms"
];
var feedIndex = 0;
var feedTickCounter = 0;

var simTime = 0;
var startRealTime = Date.now();
var uptimeHours = 847;

function updateDashboard(dt) {
    simTime += dt;
    if (simTime < 2000) return;

    // Random walk stats
    cpuVal = randomWalk(cpuVal, 15, 95, 4);
    memVal = randomWalk(memVal, 40, 88, 2);
    netVal = randomWalk(netVal, 5, 120, 8);
    diskVal = randomWalk(diskVal, 10, 60, 3);
    latencyVal = randomWalk(latencyVal, 3, 80, 5);
    errorVal = randomWalk(errorVal, 0, 5, 0.4);
    connsVal = randomWalk(connsVal, 800, 3000, 100);
    queueVal = randomWalk(queueVal, 0, 100, 8);
    cacheVal = randomWalk(cacheVal, 70, 99, 2);

    // Stat card values + progress bars
    statCpu.textContent = Math.round(cpuVal) + "%";
    statMem.textContent = Math.round(memVal) + "%";
    statNet.textContent = Math.round(netVal) + " MB/s";
    statDisk.textContent = Math.round(diskVal) + "%";

    barCpu.style.width = Math.round(cpuVal) + "%";
    barMem.style.width = Math.round(memVal) + "%";
    barNet.style.width = Math.min(Math.round(netVal / 1.2), 100) + "%";
    barDisk.style.width = Math.round(diskVal) + "%";

    // Gauge values + bars
    gLatency.textContent = Math.round(latencyVal) + "ms";
    gErrors.textContent = errorVal.toFixed(1) + "%";
    var days = Math.floor(uptimeHours / 24);
    var hrs = Math.round(uptimeHours % 24);
    gUptime.textContent = days + "d " + hrs + "h";
    gConns.textContent = Math.round(connsVal);
    gQueue.textContent = Math.round(queueVal);
    gCache.textContent = Math.round(cacheVal) + "%";

    gbLatency.style.width = Math.min(Math.round(latencyVal / 0.8), 100) + "%";
    gbErrors.style.width = Math.min(Math.round(errorVal * 20), 100) + "%";
    gbUptime.style.width = "95%";
    gbConns.style.width = Math.min(Math.round(connsVal / 30), 100) + "%";
    gbQueue.style.width = Math.round(queueVal) + "%";
    gbCache.style.width = Math.round(cacheVal) + "%";

    // Chart bars - animate heights
    for (var i = 0; i < 12; i++) {
        chartTargets[i] = randomWalk(chartTargets[i], 20, 100, 6);
        chartBars[i] = lerp(chartBars[i], chartTargets[i], 0.1);
        cbarEls[i].style.height = Math.round(chartBars[i] * 1.2) + "px";
    }

    // Clock
    var elapsed = Date.now() - startRealTime;
    var secs = Math.floor(elapsed / 1000);
    var mins = Math.floor(secs / 60);
    var hours = Math.floor(mins / 60);
    var ms = elapsed % 1000;
    clockEl.textContent =
        String(hours).padStart(2, "0") + ":" +
        String(mins % 60).padStart(2, "0") + ":" +
        String(secs % 60).padStart(2, "0") + "." +
        String(ms).padStart(3, "0");

    // Rotate feed messages through pre-existing slots
    feedTickCounter++;
    if (feedTickCounter % 30 === 0) {
        // Shift all slots down
        for (var s = 7; s > 0; s--) {
            if (feedSlots[s] && feedSlots[s - 1]) {
                feedSlots[s].textContent = feedSlots[s - 1].textContent;
            }
        }
        // New message at top
        if (feedSlots[0]) {
            feedSlots[0].textContent = feedMessages[feedIndex % feedMessages.length];
            feedIndex++;
        }
    }
}

// === Main loop ===
var lastTick = Date.now();
setInterval(function() {
    var now = Date.now();
    var dt = now - lastTick;
    lastTick = now;
    tickAnimations(dt);
    updateDashboard(dt);
}, 16);

console.log("Dashboard loaded!");
