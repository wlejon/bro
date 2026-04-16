window.views = window.views || {};
window.views.forms = {
    init: function(el) {
        var output = el.querySelector('#f-output');
        var logEl = el.querySelector('#f-log');

        function show(msg) { output.textContent = msg; }
        function log(msg, cls) {
            var d = document.createElement('div');
            d.textContent = msg;
            if (cls) d.style.color = cls;
            logEl.appendChild(d);
            logEl.scrollTop = logEl.scrollHeight;
        }

        el.querySelector('#f-name').addEventListener('input', function() { show('Name: ' + this.getAttribute('value')); });
        el.querySelector('#f-num').addEventListener('input', function() { show('Number: ' + this.getAttribute('value')); });

        var chk1 = el.querySelector('#f-chk1');
        var chk2 = el.querySelector('#f-chk2');
        function chkLog() { show('A: ' + chk1.checked + ', B: ' + chk2.checked); }
        chk1.addEventListener('change', chkLog);
        chk2.addEventListener('change', chkLog);

        var slider = el.querySelector('#f-slider');
        slider.addEventListener('input', function() {
            var v = slider.getAttribute('value');
            el.querySelector('#f-slider-val').textContent = v;
            show('Range: ' + v);
        });

        var clr = el.querySelector('#f-color');
        clr.addEventListener('change', function() {
            var v = clr.getAttribute('value');
            el.querySelector('#f-color-val').textContent = v;
            show('Color: ' + v);
        });

        el.querySelector('#f-sel').addEventListener('change', function() {
            show('Select: ' + this.getAttribute('value'));
        });

        el.querySelector('#f-btn1').addEventListener('click', function() { show('Button clicked!'); });
        el.querySelector('#f-btn2').addEventListener('click', function() { show('Submit clicked!'); });
        el.querySelector('#f-btn3').addEventListener('click', function() { show('Reset clicked!'); });

        // Clipboard events
        var clipSrc = el.querySelector('#f-clip-src');
        var clipDst = el.querySelector('#f-clip-dst');
        function clipLog(e) {
            var text = e.clipboardData ? e.clipboardData.getData('text/plain') : '';
            log(e.type + ': "' + text + '"', '#a78bfa');
        }
        clipSrc.addEventListener('copy', clipLog);
        clipSrc.addEventListener('cut', clipLog);
        clipDst.addEventListener('paste', clipLog);

        // File drop
        var fileDrop = el.querySelector('#f-file-drop');
        fileDrop.addEventListener('dragenter', function(e) { e.preventDefault(); fileDrop.className = 'drop-zone dragover'; });
        fileDrop.addEventListener('dragover', function(e) { e.preventDefault(); });
        fileDrop.addEventListener('dragleave', function() { fileDrop.className = 'drop-zone'; });
        fileDrop.addEventListener('drop', function(e) {
            e.preventDefault();
            fileDrop.className = 'drop-zone dropped';
            if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
                var names = [];
                for (var i = 0; i < e.dataTransfer.files.length; i++) names.push(e.dataTransfer.files[i].name);
                fileDrop.textContent = names.join(', ');
                log('Dropped files: ' + names.join(', '), '#38bdf8');
            }
            setTimeout(function() { fileDrop.className = 'drop-zone'; fileDrop.textContent = 'Drop files here from your file explorer'; }, 3000);
        });

        // Text drop
        var textDrop = el.querySelector('#f-text-drop');
        textDrop.addEventListener('dragenter', function(e) { e.preventDefault(); textDrop.className = 'drop-zone dragover'; });
        textDrop.addEventListener('dragover', function(e) { e.preventDefault(); });
        textDrop.addEventListener('dragleave', function() { textDrop.className = 'drop-zone'; });
        textDrop.addEventListener('drop', function(e) {
            e.preventDefault();
            textDrop.className = 'drop-zone dropped';
            var text = e.dataTransfer ? e.dataTransfer.getData('text/plain') : '';
            textDrop.textContent = text || '(no text)';
            log('Dropped text: ' + text, '#38bdf8');
            setTimeout(function() { textDrop.className = 'drop-zone'; textDrop.textContent = 'Drag text from another application here'; }, 3000);
        });

        log('Ready.', '#64748b');
    }
};
