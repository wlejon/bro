// bro engine showcase — feature demo with UI controls
(function() {
    'use strict';

    var canvas = document.getElementById('c');
    var fpsEl = document.getElementById('fps');
    var dimsEl = document.getElementById('dims');

    // --- Renderer ---
    var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: false });
    renderer.setSize(canvas.width, canvas.height, false);
    renderer.shadowMap.enabled = false;
    renderer.shadowMap.type = THREE.PCFSoftShadowMap;

    var scene = new THREE.Scene();
    scene.background = new THREE.Color(0x1a1e2e);

    var camera = new THREE.PerspectiveCamera(60, canvas.width / canvas.height, 0.1, 100);
    camera.position.set(3, 2.5, 4);
    camera.lookAt(0, 0, 0);

    // --- Lights ---
    var ambientLight = new THREE.AmbientLight(0x404040, 1.0);
    scene.add(ambientLight);

    var dirLight = new THREE.DirectionalLight(0xffffff, 3.0);
    dirLight.position.set(5, 8, 5);
    dirLight.castShadow = true;
    dirLight.shadow.mapSize.width = 1024;
    dirLight.shadow.mapSize.height = 1024;
    dirLight.shadow.camera.near = 0.5;
    dirLight.shadow.camera.far = 50;
    dirLight.shadow.camera.left = -10;
    dirLight.shadow.camera.right = 10;
    dirLight.shadow.camera.top = 10;
    dirLight.shadow.camera.bottom = -10;
    scene.add(dirLight);

    // --- Texture ---
    var textureLoader = new THREE.TextureLoader();
    var checkerTex = textureLoader.load('checker.png');

    // --- Geometries ---
    var geometries = {
        box:      new THREE.BoxGeometry(1.4, 1.4, 1.4),
        sphere:   new THREE.SphereGeometry(0.9, 32, 24),
        torus:    new THREE.TorusGeometry(0.7, 0.3, 24, 48),
        knot:     new THREE.TorusKnotGeometry(0.6, 0.2, 100, 16),
        cylinder: new THREE.CylinderGeometry(0.6, 0.6, 1.4, 32)
    };

    // --- Main material ---
    var mainMaterial = new THREE.MeshStandardMaterial({
        color: 0x4488ff,
        roughness: 0.5,
        metalness: 0.3
    });

    // --- Main object ---
    var mainMesh = new THREE.Mesh(geometries.box, mainMaterial);
    mainMesh.castShadow = true;
    mainMesh.receiveShadow = true;
    scene.add(mainMesh);

    // --- Ground plane ---
    var ground = new THREE.Mesh(
        new THREE.PlaneGeometry(20, 20),
        new THREE.MeshStandardMaterial({ color: 0x666677, roughness: 0.9 })
    );
    ground.rotation.x = -Math.PI / 2;
    ground.position.y = -1.8;
    ground.receiveShadow = true;
    ground.visible = false;
    scene.add(ground);

    // --- Extra objects for multi scene ---
    var extras = [];
    function addExtra(geo, color, x, y, z) {
        var m = new THREE.Mesh(geo,
            new THREE.MeshStandardMaterial({ color: color, roughness: 0.4, metalness: 0.5 }));
        m.position.set(x, y, z);
        m.castShadow = true;
        m.receiveShadow = true;
        m.visible = false;
        scene.add(m);
        extras.push(m);
        return m;
    }
    addExtra(new THREE.SphereGeometry(0.5, 24, 16),          0xff4444, -2.5, 0, 0);
    addExtra(new THREE.TorusGeometry(0.4, 0.15, 16, 32),     0x44ff44,  2.5, 0, 0);
    addExtra(new THREE.TorusKnotGeometry(0.35, 0.12, 64, 12),0xffaa22,  0,   0, -2.5);
    addExtra(new THREE.CylinderGeometry(0.35, 0.35, 0.8, 24),0xaa44ff,  0,   0,  2.5);

    // --- glTF ---
    var gltfModel = null;
    var gltfLoader = new THREE.GLTFLoader();
    function ensureGltf(cb) {
        if (gltfModel) { if (cb) cb(); return; }
        gltfLoader.load('cube.glb', function(gltf) {
            gltfModel = gltf.scene;
            gltfModel.visible = false;
            gltfModel.traverse(function(c) {
                if (c.isMesh) { c.castShadow = true; c.receiveShadow = true; }
            });
            scene.add(gltfModel);
            console.log('glTF model loaded');
            if (cb) cb();
        });
    }

    // --- State ---
    var speed = 1.0;
    var paused = false;
    var currentScene = 'pbr';

    // --- Scene management ---
    function clearAll() {
        mainMesh.visible = false;
        mainMesh.position.set(0, 0, 0);
        ground.visible = false;
        renderer.shadowMap.enabled = false;
        for (var i = 0; i < extras.length; i++) extras[i].visible = false;
        if (gltfModel) gltfModel.visible = false;
    }

    var scenes = {
        pbr: function() {
            clearAll();
            mainMesh.visible = true;
            mainMaterial.map = null;
            mainMaterial.needsUpdate = true;
        },
        textured: function() {
            clearAll();
            mainMesh.visible = true;
            mainMaterial.map = checkerTex;
            mainMaterial.needsUpdate = true;
        },
        shadows: function() {
            clearAll();
            mainMesh.visible = true;
            ground.visible = true;
            renderer.shadowMap.enabled = true;
            mainMaterial.map = null;
            mainMaterial.needsUpdate = true;
        },
        gltf: function() {
            clearAll();
            mainMesh.visible = true;
            mainMesh.position.set(2.5, 0, 0);
            ensureGltf(function() {
                gltfModel.visible = true;
                gltfModel.scale.set(2, 2, 2);
            });
        },
        multi: function() {
            clearAll();
            mainMesh.visible = true;
            ground.visible = true;
            renderer.shadowMap.enabled = true;
            mainMaterial.map = checkerTex;
            mainMaterial.needsUpdate = true;
            for (var i = 0; i < extras.length; i++) extras[i].visible = true;
        }
    };

    // --- UI helpers ---
    // Use inline styles for active state because litehtml doesn't re-evaluate
    // CSS class selectors after className changes at runtime.
    var ACTIVE_BG = '#4488bb';
    var ACTIVE_BORDER = '#66aaff';
    var NORMAL_BG = '#2a3a50';
    var NORMAL_BORDER = '#444455';

    function $(id) { return document.getElementById(id); }

    function styleActive(el, active) {
        if (!el) return;
        el.style.backgroundColor = active ? ACTIVE_BG : NORMAL_BG;
        el.style.borderColor = active ? ACTIVE_BORDER : NORMAL_BORDER;
    }

    function setActive(groupId, btnId) {
        var g = $(groupId);
        if (!g) return;
        var kids = g.children;
        for (var i = 0; i < kids.length; i++) {
            styleActive(kids[i], kids[i].id === btnId);
        }
    }

    function on(id, fn) {
        var el = $(id);
        if (el) el.addEventListener('click', fn);
    }

    // --- Scene buttons ---
    on('btn-pbr',      function() { currentScene = 'pbr';      scenes.pbr();      setActive('scene-btns', 'btn-pbr'); });
    on('btn-textured', function() { currentScene = 'textured'; scenes.textured(); setActive('scene-btns', 'btn-textured'); });
    on('btn-shadows',  function() { currentScene = 'shadows';  scenes.shadows();  setActive('scene-btns', 'btn-shadows'); });
    on('btn-gltf',     function() { currentScene = 'gltf';     scenes.gltf();     setActive('scene-btns', 'btn-gltf'); });
    on('btn-multi',    function() { currentScene = 'multi';    scenes.multi();    setActive('scene-btns', 'btn-multi'); });

    // --- Geometry buttons ---
    on('btn-box',      function() { mainMesh.geometry = geometries.box;      setActive('geo-btns', 'btn-box'); });
    on('btn-sphere',   function() { mainMesh.geometry = geometries.sphere;   setActive('geo-btns', 'btn-sphere'); });
    on('btn-torus',    function() { mainMesh.geometry = geometries.torus;    setActive('geo-btns', 'btn-torus'); });
    on('btn-knot',     function() { mainMesh.geometry = geometries.knot;     setActive('geo-btns', 'btn-knot'); });
    on('btn-cylinder', function() { mainMesh.geometry = geometries.cylinder; setActive('geo-btns', 'btn-cylinder'); });

    // --- Color buttons ---
    on('btn-color-blue',  function() { mainMaterial.color.setHex(0x4488ff); });
    on('btn-color-red',   function() { mainMaterial.color.setHex(0xff4444); });
    on('btn-color-green', function() { mainMaterial.color.setHex(0x44ff44); });
    on('btn-color-gold',  function() { mainMaterial.color.setHex(0xddaa22); });
    on('btn-color-white', function() { mainMaterial.color.setHex(0xdddddd); });

    // --- Material property buttons ---
    function setRoughGroup(active) {
        styleActive($('btn-rough-lo'), active === 'lo');
        styleActive($('btn-rough-mid'), active === 'mid');
        styleActive($('btn-rough-hi'), active === 'hi');
    }
    on('btn-rough-lo',  function() { mainMaterial.roughness = 0.05; setRoughGroup('lo'); });
    on('btn-rough-mid', function() { mainMaterial.roughness = 0.5;  setRoughGroup('mid'); });
    on('btn-rough-hi',  function() { mainMaterial.roughness = 0.95; setRoughGroup('hi'); });

    function setMetalGroup(active) {
        styleActive($('btn-metal-lo'), active === 'lo');
        styleActive($('btn-metal-mid'), active === 'mid');
        styleActive($('btn-metal-hi'), active === 'hi');
    }
    on('btn-metal-lo',  function() { mainMaterial.metalness = 0.0; setMetalGroup('lo'); });
    on('btn-metal-mid', function() { mainMaterial.metalness = 0.5; setMetalGroup('mid'); });
    on('btn-metal-hi',  function() { mainMaterial.metalness = 1.0; setMetalGroup('hi'); });

    // --- Animation buttons ---
    function setSpeedGroup(active) {
        styleActive($('btn-slow'), active === 'slow');
        styleActive($('btn-normal'), active === 'normal');
        styleActive($('btn-fast'), active === 'fast');
    }
    on('btn-slow',   function() { speed = 0.3;  setSpeedGroup('slow'); });
    on('btn-normal', function() { speed = 1.0;  setSpeedGroup('normal'); });
    on('btn-fast',   function() { speed = 3.0;  setSpeedGroup('fast'); });
    on('btn-pause',  function() {
        paused = !paused;
        styleActive($('btn-pause'), paused);
        $('btn-pause').textContent = paused ? 'Resume' : 'Pause';
    });
    on('btn-reset', function() {
        mainMesh.rotation.set(0, 0, 0);
        for (var i = 0; i < extras.length; i++) extras[i].rotation.set(0, 0, 0);
        if (gltfModel) gltfModel.rotation.set(0, 0, 0);
    });

    // --- Lighting buttons ---
    function setLightGroup(active) {
        styleActive($('btn-light-dim'), active === 'dim');
        styleActive($('btn-light-normal'), active === 'normal');
        styleActive($('btn-light-bright'), active === 'bright');
    }
    on('btn-light-dim',    function() { dirLight.intensity = 1.0; ambientLight.intensity = 0.3; setLightGroup('dim'); });
    on('btn-light-normal', function() { dirLight.intensity = 3.0; ambientLight.intensity = 1.0; setLightGroup('normal'); });
    on('btn-light-bright', function() { dirLight.intensity = 6.0; ambientLight.intensity = 2.0; setLightGroup('bright'); });

    // --- Display buttons ---
    on('btn-wireframe', function() {
        mainMaterial.wireframe = !mainMaterial.wireframe;
        styleActive($('btn-wireframe'), mainMaterial.wireframe);
    });
    on('btn-bg-dark', function() {
        scene.background = new THREE.Color(0x1a1e2e);
        styleActive($('btn-bg-dark'), true);
        styleActive($('btn-bg-light'), false);
    });
    on('btn-bg-light', function() {
        scene.background = new THREE.Color(0xc0c8d0);
        styleActive($('btn-bg-dark'), false);
        styleActive($('btn-bg-light'), true);
    });

    // --- Set initial active states via inline styles ---
    setActive('scene-btns', 'btn-pbr');
    setActive('geo-btns', 'btn-box');
    setRoughGroup('mid');
    setMetalGroup('lo');
    setSpeedGroup('normal');
    setLightGroup('normal');
    styleActive($('btn-bg-dark'), true);

    // --- Resize handler ---
    window.addEventListener('resize', function() {
        var w = window.innerWidth;
        var h = window.innerHeight;
        camera.aspect = w / h;
        camera.updateProjectionMatrix();
        renderer.setSize(w, h, false);
        if (dimsEl) dimsEl.textContent = w + 'x' + h;
    });

    // --- FPS ---
    var frameCount = 0;
    var lastFpsTime = performance.now();

    // --- Animate ---
    function animate() {
        requestAnimationFrame(animate);

        if (!paused) {
            var s = speed * 0.01;
            mainMesh.rotation.x += s;
            mainMesh.rotation.y += s * 2;
            for (var i = 0; i < extras.length; i++) {
                extras[i].rotation.y += s * (1.5 + i * 0.5);
                extras[i].rotation.x += s * (0.5 + i * 0.3);
            }
            if (gltfModel && gltfModel.visible) {
                gltfModel.rotation.y += s * 1.5;
            }
        }

        renderer.render(scene, camera);

        frameCount++;
        var now = performance.now();
        if (now - lastFpsTime >= 1000) {
            if (fpsEl) fpsEl.textContent = String(frameCount);
            frameCount = 0;
            lastFpsTime = now;
        }
    }

    // Start
    scenes.pbr();
    console.log('Showcase ready');
    animate();
})();
