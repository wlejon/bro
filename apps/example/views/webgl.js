window.views = window.views || {};
window.views.webgl = {
    running: false,
    init: function(el) {
        this.running = true;

        if (!window.THREE) {
            var src = require('fs').readFileSync('lib/three.min.js', 'utf-8');
            (0, eval)(src);
        }
        // Defer setup until after first layout so canvas clientWidth/Height
        // reflects the CSS-stretched size rather than the 300x150 default.
        var self = this;
        requestAnimationFrame(function() { if (self.running) self._setup(el); });
    },
    _setup: function(el) {
        var self = this;
        var fs = require('fs');
        var canvas = el.querySelector('#wgl-canvas');
        if (!canvas) return;

        function measure() {
            var r = canvas.getBoundingClientRect();
            return { w: Math.floor(r.width) || 0, h: Math.floor(r.height) || 0 };
        }

        var m = measure();
        // Layout may not have run yet for a freshly-parsed canvas; fall back
        // to a sensible default and fix up on the next frame.
        var w = m.w || 1024;
        var h = m.h || 768;
        canvas.width = w;
        canvas.height = h;

        var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: true });
        renderer.setSize(w, h, false);
        self._renderer = renderer;

        var scene = new THREE.Scene();
        scene.background = new THREE.Color(0.1, 0.1, 0.15);

        var camera = new THREE.PerspectiveCamera(60, w / h, 0.1, 100);
        camera.position.set(0, 1.5, 2.0);
        camera.lookAt(0, 0, 0);

        var vertexShader = fs.readFileSync('shaders/custom.vert', 'utf-8');
        var fragmentShader = fs.readFileSync('shaders/custom.frag', 'utf-8');

        var geometry = new THREE.PlaneGeometry(4, 4, 128, 128);
        geometry.rotateX(-Math.PI / 2);
        var count = geometry.attributes.position.count;

        var customPos = new Float32Array(count * 3);
        var customColor = new Float32Array(count * 4);
        for (var i = 0; i < count; i++) {
            customPos[i * 3] = (Math.random() - 0.5) * 0.05;
            customPos[i * 3 + 1] = (Math.random() - 0.5) * 0.05;
            customPos[i * 3 + 2] = (Math.random() - 0.5) * 0.05;
            var x = geometry.attributes.position.getX(i);
            var z = geometry.attributes.position.getZ(i);
            customColor[i * 4] = (x + 2) / 4;
            customColor[i * 4 + 1] = 0.5;
            customColor[i * 4 + 2] = (z + 2) / 4;
            customColor[i * 4 + 3] = 1.0;
        }
        geometry.setAttribute('aCustomPos', new THREE.BufferAttribute(customPos, 3));
        geometry.setAttribute('aCustomColor', new THREE.BufferAttribute(customColor, 4));

        var material = new THREE.ShaderMaterial({
            vertexShader: vertexShader,
            fragmentShader: fragmentShader,
            uniforms: { uTime: { value: 0 }, uScale: { value: 0.4 } },
            side: THREE.DoubleSide
        });

        scene.add(new THREE.Mesh(geometry, material));

        var clock = new THREE.Clock();
        function render() {
            if (!self.running) return;
            // Re-measure each frame in case layout produced a different size
            // than we had at construction (common right after innerHTML swap).
            var cur = measure();
            if (cur.w > 0 && cur.h > 0 && (cur.w !== canvas.width || cur.h !== canvas.height)) {
                canvas.width = cur.w;
                canvas.height = cur.h;
                renderer.setSize(cur.w, cur.h, false);
                camera.aspect = cur.w / cur.h;
                camera.updateProjectionMatrix();
            }
            material.uniforms.uTime.value = clock.getElapsedTime() * 0.5;
            renderer.render(scene, camera);
            requestAnimationFrame(render);
        }
        requestAnimationFrame(render);
    },
    destroy: function() {
        this.running = false;
        this._renderer = null;
    }
};
