window.views = window.views || {};
window.views.jquery = {
    _interval: null,
    init: function(el) {
        if (!window.jQuery) {
            var src = require('fs').readFileSync('lib/jquery-3.7.1.slim.min.js', 'utf-8');
            (0, eval)(src);
        }
        this._setup(el);
    },
    _setup: function(el) {
        var $ = window.jQuery;
        var self = this;
        var paused = false;
        var updateCount = 0;
        var cardCount = 0;

        var metrics = {
            cpu: { value: 30, target: 30 },
            mem: { value: 50, target: 50 },
            net: { value: 20, target: 20 }
        };

        var feedMsgs = [
            'Connection established', 'Data synced', 'Cache cleared',
            'New session started', 'Backup complete', 'Config updated',
            'Request processed', 'Index rebuilt', 'Queue flushed'
        ];

        $('#jq-version').text('jQuery ' + $.fn.jquery);

        function lerp(a, b, t) { return a + (b - a) * t; }

        function tick() {
            if (paused) return;
            updateCount++;

            for (var k in metrics) {
                var m = metrics[k];
                if (Math.random() < 0.1) m.target = Math.random() * 100;
                m.value = lerp(m.value, m.target, 0.1);
                var pct = Math.round(m.value);
                $('#jq-' + k + '-bar').css('width', pct + '%');
                $('#jq-' + k + '-val').text(pct + '%');
            }

            if (Math.random() < 0.15) {
                var msg = feedMsgs[Math.floor(Math.random() * feedMsgs.length)];
                var time = new Date().toLocaleTimeString();
                $('<li>').text(time + ' — ' + msg).prependTo('#jq-feed');
                var feed = $('#jq-feed');
                if (feed.children().length > 20) feed.children().last().remove();
            }

            $('#jq-update-count').text(updateCount + ' updates');
        }

        self._interval = setInterval(tick, 100);

        $('#jq-pause').on('click', function() {
            paused = !paused;
            $(this).text(paused ? 'Resume' : 'Pause');
            $('#jq-status').text(paused ? 'Paused' : 'Running')
                .css('background', paused ? '#713f12' : '#166534')
                .css('color', paused ? '#fbbf24' : '#4ade80');
        });

        $('#jq-clear').on('click', function() { $('#jq-feed').empty(); });

        $('#jq-add-card').on('click', function() {
            cardCount++;
            var val = Math.floor(Math.random() * 1000);
            $('<div class="jq-card">').html(
                '<span class="name">Card ' + cardCount + '</span> <span class="val">' + val + '</span>'
            ).appendTo('#jq-cards');
        });

        for (var i = 0; i < 4; i++) {
            cardCount++;
            $('<div class="jq-card">').html(
                '<span class="name">Card ' + cardCount + '</span> <span class="val">' + Math.floor(Math.random() * 1000) + '</span>'
            ).appendTo('#jq-cards');
        }
    },
    destroy: function() {
        if (this._interval) { clearInterval(this._interval); this._interval = null; }
    }
};
