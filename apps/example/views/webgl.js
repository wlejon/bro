window.views = window.views || {};
window.views.webgl = {
    running: false,
    init: function(el) {
        this.running = true;

        if (!window.THREE) {
            var src = require('fs').readFileSync('lib/three.min.js', 'utf-8');
            (0, eval)(src);
        }
        this._setup(el);
    },
    _setup: function(el) {
        var self = this;
        var fs = require('fs');
        var canvas = el.querySelector('#wgl-canvas');

        var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: true });
        renderer.setSize(canvas.width, canvas.height);
        self._renderer = renderer;

        var scene = new THREE.Scene();
        scene.background = new THREE.Color(0.1, 0.1, 0.15);

        var camera = new THREE.PerspectiveCamera(60, canvas.width / canvas.height, 0.1, 100);
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
