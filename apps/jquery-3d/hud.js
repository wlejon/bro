// jQuery HUD overlay — updates metrics on top of 3D scene
(function($) {
    'use strict';

    var cpu = { value: 30, target: 30 };
    var mem = { value: 50, target: 50 };
    var tickCount = 0;
    var fpsFrames = 0;
    var lastFpsTime = performance.now();
    var lastUpdateTime = 0;

    var $cpuBar = $('#cpu-bar'), $cpuVal = $('#cpu-val');
    var $memBar = $('#mem-bar'), $memVal = $('#mem-val');
    var $info = $('#info'), $feed = $('#feed');

    var lastCpuPct = -1, lastMemPct = -1;

    var messages = [
        'Cube orbit updated', 'Light position shifted',
        'Camera angle recalculated', 'Scene rendered',
        'Geometry buffer uploaded', 'Shader compiled',
        'Texture sampled', 'Shadow map refreshed'
    ];
    var msgIdx = 0;

    function tick() {
        var now = performance.now();

        // Throttle DOM updates to ~30fps
        if (now - lastUpdateTime >= 33) {
            lastUpdateTime = now;
            tickCount++;

            if (Math.random() < 0.05) cpu.target = Math.floor(Math.random() * 90) + 5;
            if (Math.random() < 0.05) mem.target = Math.floor(Math.random() * 90) + 5;
            cpu.value += (cpu.target - cpu.value) * 0.15;
            mem.value += (mem.target - mem.value) * 0.08;

            var cpuPct = Math.round(cpu.value);
            var memPct = Math.round(mem.value);

            if (cpuPct !== lastCpuPct) {
                lastCpuPct = cpuPct;
                $cpuBar.css('width', cpuPct + '%');
                $cpuVal.text(cpuPct + '%');
            }
            if (memPct !== lastMemPct) {
                lastMemPct = memPct;
                $memBar.css('width', memPct + '%');
                $memVal.text(memPct + '%');
            }

            // Add feed item every ~10 ticks
            if (tickCount % 10 === 0) {
                var msg = messages[msgIdx % messages.length];
                msgIdx++;
                var $item = $('<div>').addClass('feed-item').text(msg);
                $feed.prepend($item);
                var $items = $feed.children();
                if ($items.length > 5) $items.last().remove();
            }
        }

        // FPS counter (updates every second)
        fpsFrames++;
        var elapsed = now - lastFpsTime;
        if (elapsed >= 1000) {
            var fps = Math.round(fpsFrames * 1000 / elapsed);
            $info.text('HUD: ' + fps + ' fps | cubes: 12 | tick: ' + tickCount);
            fpsFrames = 0;
            lastFpsTime = now;
        }

        requestAnimationFrame(tick);
    }

    console.log('jQuery HUD initialized');
    requestAnimationFrame(tick);

})(jQuery);
