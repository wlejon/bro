window.views = window.views || {};
window.views.network = {
    init: function(el) {
        var logEl = el.querySelector('#net-log');
        var statusEl = el.querySelector('#net-status');
        var connsEl = el.querySelector('#net-conns');

        function log(msg) {
            var d = document.createElement('div');
            d.textContent = '[' + new Date().toLocaleTimeString() + '] ' + msg;
            logEl.appendChild(d);
            logEl.scrollTop = logEl.scrollHeight;
        }

        function updateConns() {
            var c = bro.net.connections();
            connsEl.textContent = c.length + (c.length ? ' — ' + c.join(', ') : '');
        }

        el.querySelector('#net-init').addEventListener('click', function() {
            var ok = bro.net.init();
            log(ok ? 'Network initialized!' : 'Init failed');
            statusEl.textContent = ok ? 'Initialized' : 'Init failed';
            if (ok) statusEl.className = 'status-label connected';
        });

        el.querySelector('#net-host').addEventListener('click', function() {
            var ok = bro.net.host(27015);
            log(ok ? 'Hosting on port 27015' : 'Host failed');
            if (ok) statusEl.textContent = 'Hosting on :27015';
        });

        el.querySelector('#net-connect').addEventListener('click', function() {
            var addr = el.querySelector('#net-addr').value;
            var ok = bro.net.connect(addr);
            log(ok ? 'Connecting to ' + addr + '...' : 'Connect failed');
        });

        el.querySelector('#net-send').addEventListener('click', function() {
            var conns = bro.net.connections();
            if (conns.length === 0) { log('No connections'); return; }
            var msg = 'Hello! Time: ' + Date.now();
            var data = new TextEncoder().encode(msg);
            for (var i = 0; i < conns.length; i++) {
                bro.net.send(conns[i], data.buffer, true);
                log('Sent to ' + conns[i] + ': ' + msg);
            }
        });

        el.querySelector('#net-broadcast').addEventListener('click', function() {
            var msg = 'Broadcast: ' + Date.now();
            bro.net.broadcast(new TextEncoder().encode(msg).buffer, true);
            log('Broadcast: ' + msg);
        });

        el.querySelector('#net-close').addEventListener('click', function() {
            bro.net.close();
            log('Closed');
            statusEl.textContent = 'Closed';
            statusEl.className = 'status-label';
            updateConns();
        });

        bro.net.onconnect = function(connId) {
            log('Connected: ' + connId);
            updateConns();
            setTimeout(function() {
                var s = bro.net.stats(connId);
                if (s) log('Stats: ping=' + s.ping + 'ms loss=' + (s.packetLoss * 100).toFixed(1) + '%');
            }, 1000);
        };

        bro.net.ondisconnect = function(connId, reason) {
            log('Disconnected: ' + connId + ' (reason: ' + reason + ')');
            updateConns();
        };

        bro.net.onmessage = function(connId, data) {
            log('From ' + connId + ': ' + new TextDecoder().decode(data));
        };

        log('Ready. Click "Init Network" to start.');
    }
};
