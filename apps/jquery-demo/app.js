// ==========================================================================
// jQuery Live Dashboard — exercises jQuery extensively with continuous updates
// ==========================================================================

(function($) {
    'use strict';

    // -- State ---------------------------------------------------------------
    var paused = false;
    var updateCount = 0;
    var tickCount = 0;
    var fpsFrames = 0;
    var lastFpsTime = performance.now();

    // Simulated metrics with momentum
    var metrics = {
        cpu:  { value: 30, target: 30, speed: 0.15 },
        mem:  { value: 50, target: 50, speed: 0.08 },
        net:  { value: 20, target: 20, speed: 0.12 },
        disk: { value: 40, target: 40, speed: 0.05 }
    };

    // History for sparklines
    var history = { cpu: [], mem: [], net: [] };
    var maxHistory = 40;

    // Feed messages
    var feedMessages = [
        { type: 'info',    msg: 'Connection established to node-04' },
        { type: 'info',    msg: 'Cache warmed: 2,048 entries loaded' },
        { type: 'success', msg: 'Deployment v3.2.1 completed' },
        { type: 'warn',    msg: 'Memory pressure on worker-07 (82%)' },
        { type: 'info',    msg: 'Auto-scaling: +2 instances added' },
        { type: 'error',   msg: 'Timeout on healthcheck /api/status' },
        { type: 'success', msg: 'Database migration #47 applied' },
        { type: 'info',    msg: 'SSL certificate renewed (expires 2027)' },
        { type: 'warn',    msg: 'Disk I/O latency spike: 45ms p99' },
        { type: 'success', msg: 'Backup snapshot completed (12.4 GB)' },
        { type: 'info',    msg: 'CDN cache invalidated for /assets/*' },
        { type: 'error',   msg: 'Rate limit exceeded from 10.0.3.42' },
        { type: 'info',    msg: 'New node-09 joined the cluster' },
        { type: 'warn',    msg: 'GC pause 120ms on worker-02' },
        { type: 'success', msg: 'Canary deploy passed all checks' },
        { type: 'info',    msg: 'Log rotation: archived 340 MB' },
        { type: 'error',   msg: 'Connection refused: redis-primary' },
        { type: 'success', msg: 'Failover to redis-secondary complete' },
        { type: 'warn',    msg: 'Queue depth > 1000 on ingest-03' },
        { type: 'info',    msg: 'Config reload: 3 services updated' }
    ];
    var feedIndex = 0;

    // Card data
    var cardData = [
        { title: 'Requests',  value: 12847, unit: '/s' },
        { title: 'Latency',   value: 23,    unit: 'ms' },
        { title: 'Errors',    value: 7,     unit: '/m' },
        { title: 'Uptime',    value: 99.97, unit: '%' },
        { title: 'Throughput', value: 842,  unit: 'MB/s' },
        { title: 'Connections', value: 1563, unit: '' }
    ];

    // Task templates
    var taskTemplates = [
        'Review PR #', 'Update docs for ', 'Fix flaky test in ',
        'Optimize query in ', 'Add metrics to ', 'Refactor ',
        'Deploy ', 'Monitor ', 'Debug ', 'Benchmark '
    ];
    var taskSuffixes = [
        'auth service', 'API gateway', 'worker pool',
        'cache layer', 'search index', 'data pipeline',
        'user module', 'billing system', 'notification service'
    ];
    var taskId = 0;

    // -- Utility -------------------------------------------------------------
    function rand(min, max) { return Math.floor(Math.random() * (max - min + 1)) + min; }
    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
    function formatTime() {
        var t = Math.floor(performance.now() / 1000);
        var h = Math.floor(t / 3600) % 24;
        var m = Math.floor(t / 60) % 60;
        var s = t % 60;
        return (h < 10 ? '0' : '') + h + ':' +
               (m < 10 ? '0' : '') + m + ':' +
               (s < 10 ? '0' : '') + s;
    }

    // -- Stats ---------------------------------------------------------------
    function updateMetrics() {
        $.each(metrics, function(key, m) {
            // Occasionally pick a new target
            if (Math.random() < 0.05) {
                m.target = rand(5, 95);
            }
            // Ease toward target
            m.value += (m.target - m.value) * m.speed;
            m.value = clamp(m.value, 0, 100);

            var pct = Math.round(m.value);
            $('#' + key + '-bar').css('width', pct + '%');
            $('#' + key + '-val').text(pct + '%');
        });

        // Record history
        history.cpu.push(Math.round(metrics.cpu.value));
        history.mem.push(Math.round(metrics.mem.value));
        history.net.push(Math.round(metrics.net.value));
        if (history.cpu.length > maxHistory) {
            history.cpu.shift();
            history.mem.shift();
            history.net.shift();
        }
    }

    // -- Sparklines ----------------------------------------------------------
    function renderSparklines() {
        $.each(['cpu', 'mem', 'net'], function(_, key) {
            var $container = $('#spark-' + key);
            $container.empty();
            var data = history[key];
            for (var i = 0; i < data.length; i++) {
                var h = Math.max(1, Math.round(data[i] / 100 * 28));
                $container.append(
                    $('<div>').addClass('spark-bar ' + key)
                              .css('height', h + 'px')
                );
            }
        });
    }

    // -- Feed ----------------------------------------------------------------
    function addFeedItem() {
        var entry = feedMessages[feedIndex % feedMessages.length];
        feedIndex++;

        var $item = $('<li>').addClass('feed-item ' + entry.type + ' feed-new');
        $item.append($('<span>').addClass('time').text(formatTime()));
        $item.append($('<span>').addClass('msg').text(entry.msg));

        $('#feed-list').prepend($item);

        // Remove highlight after a moment
        setTimeout(function() {
            $item.removeClass('feed-new');
        }, 800);

        // Keep feed to 8 items max
        var $items = $('#feed-list').children();
        if ($items.length > 8) {
            $items.last().remove();
        }
    }

    // -- Cards ---------------------------------------------------------------
    function initCards() {
        var $container = $('#card-container');
        $.each(cardData, function(i, card) {
            var $card = $('<div>').addClass('card').attr('data-idx', i);
            $card.append($('<div>').addClass('card-title').text(card.title));
            $card.append($('<div>').addClass('card-value').text(card.value + card.unit));
            $card.append($('<div>').addClass('card-delta flat').text('--'));

            // Click to highlight
            $card.on('click', function() {
                $(this).toggleClass('highlight');
            });

            $container.append($card);
        });
    }

    function updateCards() {
        $('#card-container .card').each(function() {
            var $card = $(this);
            var idx = parseInt($card.attr('data-idx'));
            if (isNaN(idx) || idx >= cardData.length) return;

            var card = cardData[idx];
            var oldVal = card.value;

            // Simulate value changes
            var delta;
            if (card.title === 'Uptime') {
                delta = 0;
            } else if (card.title === 'Errors') {
                delta = rand(-2, 3);
            } else if (card.title === 'Latency') {
                delta = rand(-5, 5);
            } else {
                delta = rand(-50, 50);
            }

            card.value = Math.max(0, card.value + delta);
            $card.find('.card-value').text(card.value + card.unit);

            // Delta indicator
            var $delta = $card.find('.card-delta');
            if (delta > 0) {
                $delta.text('+' + delta).removeClass('down flat').addClass('up');
            } else if (delta < 0) {
                $delta.text(String(delta)).removeClass('up flat').addClass('down');
            } else {
                $delta.text('--').removeClass('up down').addClass('flat');
            }
        });
    }

    function addNewCard() {
        var names = ['Workers', 'Queued', 'Pending', 'Active', 'Cached', 'Retries'];
        var name = names[rand(0, names.length - 1)];
        var val = rand(10, 999);

        var idx = cardData.length;
        cardData.push({ title: name, value: val, unit: '' });

        var $card = $('<div>').addClass('card highlight').attr('data-idx', idx);
        $card.append($('<div>').addClass('card-title').text(name));
        $card.append($('<div>').addClass('card-value').text(String(val)));
        $card.append($('<div>').addClass('card-delta flat').text('new'));
        $card.on('click', function() { $(this).toggleClass('highlight'); });

        $('#card-container').append($card);

        // Remove highlight after 2s
        setTimeout(function() { $card.removeClass('highlight'); }, 2000);
    }

    function sortCards() {
        var $container = $('#card-container');
        var $cards = $container.children('.card');
        var arr = [];
        $cards.each(function() { arr.push($(this)); });

        arr.sort(function(a, b) {
            return a.find('.card-title').text().localeCompare(b.find('.card-title').text());
        });

        $container.empty();
        $.each(arr, function(_, $c) { $container.append($c); });
    }

    function shuffleCards() {
        var $container = $('#card-container');
        var $cards = $container.children('.card');
        var arr = [];
        $cards.each(function() { arr.push($(this)); });

        // Fisher-Yates
        for (var i = arr.length - 1; i > 0; i--) {
            var j = rand(0, i);
            var tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
        }

        $container.empty();
        $.each(arr, function(_, $c) { $container.append($c); });
    }

    // -- Todos ---------------------------------------------------------------
    function addTodo() {
        taskId++;
        var text = taskTemplates[rand(0, taskTemplates.length - 1)] +
                   taskSuffixes[rand(0, taskSuffixes.length - 1)];

        var $li = $('<li>').addClass('todo-item').attr('data-id', taskId);
        var $text = $('<span>').addClass('todo-text').text(text);
        var $done = $('<span>').addClass('todo-btn').text('[done]');
        var $del  = $('<span>').addClass('todo-btn').text('[x]');

        $done.on('click', function() {
            $(this).closest('.todo-item').toggleClass('done');
            updateTodoCount();
        });

        $del.on('click', function() {
            $(this).closest('.todo-item').remove();
            updateTodoCount();
        });

        $li.append($text).append($done).append($del);
        $('#todo-list').append($li);
        updateTodoCount();
    }

    function updateTodoCount() {
        var total = $('#todo-list .todo-item').length;
        var done = $('#todo-list .todo-item.done').length;
        $('#todo-count').text(done + '/' + total + ' done');
    }

    // -- Clock & FPS ---------------------------------------------------------
    function updateClock() {
        $('#clock').text(formatTime());
    }

    function updateFps() {
        fpsFrames++;
        var now = performance.now();
        var elapsed = now - lastFpsTime;
        if (elapsed >= 1000) {
            var fps = Math.round(fpsFrames * 1000 / elapsed);
            $('#fps-display').text(fps + ' fps');
            fpsFrames = 0;
            lastFpsTime = now;
        }
    }

    // -- Main tick -----------------------------------------------------------
    function tick() {
        if (!paused) {
            tickCount++;
            updateCount++;

            updateMetrics();
            updateClock();

            // Stagger updates for visual interest
            if (tickCount % 3 === 0) addFeedItem();
            if (tickCount % 2 === 0) updateCards();
            if (tickCount % 4 === 0) renderSparklines();
            if (tickCount % 15 === 0) addTodo();

            // Periodic highlights
            if (tickCount % 10 === 0) {
                var $panels = $('.panel');
                var idx = rand(0, $panels.length - 1);
                // brief border flash on a random panel
                var $p = $panels.eq(idx);
                $p.css('border-color', '#e94560');
                setTimeout(function() {
                    $p.css('border-color', '');
                }, 500);
            }

            $('#update-count').text(updateCount + ' updates');
        }

        updateFps();
        requestAnimationFrame(tick);
    }

    // -- Event handlers ------------------------------------------------------
    function setupControls() {
        $('#btn-theme').on('click', function() {
            $('body').toggleClass('light');
            $(this).toggleClass('active');
        });

        $('#btn-pause').on('click', function() {
            paused = !paused;
            $(this).text(paused ? 'Resume' : 'Pause');
            $(this).toggleClass('active', paused);
            $('#status')
                .toggleClass('status-ok', !paused)
                .toggleClass('status-paused', paused)
                .text(paused ? 'Paused' : 'Running');
        });

        $('#btn-clear').on('click', function() {
            $('#feed-list').empty();
        });

        $('#btn-add').on('click', function() {
            addNewCard();
        });

        $('#btn-sort').on('click', function() {
            sortCards();
        });

        $('#btn-shuffle').on('click', function() {
            shuffleCards();
        });

        $('#btn-add-todo').on('click', function() {
            addTodo();
        });
    }

    // -- Init ----------------------------------------------------------------
    function init() {
        $('#jquery-version').text('jQuery ' + $.fn.jquery.split(' ')[0]);

        initCards();
        setupControls();

        // Seed some initial data
        for (var i = 0; i < 5; i++) {
            updateMetrics();
        }
        renderSparklines();
        addFeedItem();
        addFeedItem();
        addFeedItem();
        addTodo();
        addTodo();
        addTodo();
        updateClock();

        console.log('jQuery Live Dashboard initialized');
        console.log('jQuery version: ' + $.fn.jquery);

        // Start the main loop
        requestAnimationFrame(tick);
    }

    // Go!
    init();

})(jQuery);
