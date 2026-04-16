window.views = window.views || {};
window.views.events = {
    init: function(el) {
        var logEl = el.querySelector('#ev-log');
        function log(msg) {
            var d = document.createElement('div');
            d.textContent = msg;
            logEl.appendChild(d);
            logEl.scrollTop = logEl.scrollHeight;
        }

        el.querySelector('#ev-listener-card').addEventListener('click', function() {
            document.getElementById('ev-listener-box').classList.toggle('active');
            log('addEventListener on card fired');
        });

        el.querySelector('#ev-direct-box').addEventListener('click', function() {
            this.classList.toggle('active');
            log('addEventListener directly on box');
        });

        el.querySelector('#ev-text-card').addEventListener('click', function() {
            var s = document.getElementById('ev-text-label');
            s.textContent = s.textContent === 'OFF' ? 'ON' : 'OFF';
            log('Text toggled: ' + s.textContent);
        });

        var tb = el.querySelector('#ev-trans-box');
        tb.addEventListener('transitionstart', function(e) {
            log('transitionstart: ' + e.propertyName);
        });
        tb.addEventListener('transitionend', function(e) {
            log('transitionend: ' + e.propertyName + ' (' + e.elapsedTime + 's)');
        });

        var ab = el.querySelector('#ev-anim-box');
        ab.addEventListener('animationstart', function(e) {
            log('animationstart: ' + e.animationName);
        });
        ab.addEventListener('animationiteration', function(e) {
            log('animationiteration (' + e.elapsedTime + 's)');
        });
        ab.addEventListener('animationend', function(e) {
            log('animationend (' + e.elapsedTime + 's)');
        });

        log('Ready. Click boxes or hover the transition box.');
    }
};
