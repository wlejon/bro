window.views = window.views || {};
window.views.mesh = {
    running: false,
    _handler: null,
    init: function(el) {
        var self = this;
        self.running = true;

        var canvas = el.querySelector('#mesh-canvas');
        canvas.width = canvas.clientWidth || 1024;
        canvas.height = canvas.clientHeight || 768;
        var scene = canvas.getContext('scene');
        var info = el.querySelector('#mesh-info');
        var paused = false;
        var time = 0;
        var currentScene = 0;
        var sceneNodes = [];

        function log(msg) { info.textContent = msg; }

        function clearScene() {
            for (var i = 0; i < sceneNodes.length; i++) sceneNodes[i].destroy();
            sceneNodes = [];
        }
        self._clearScene = clearScene;

        function addMesh(mesh, opts) {
            opts = opts || {};
            opts.data = mesh;
            var node = scene.createMesh(opts);
            sceneNodes.push(node);
            return node;
        }

        function addGround(w) {
            addMesh(Mesh.plane(w || 10, 8), { y: -0.01, color: '#2c3e50' });
        }

        function cam(pos, target) {
            scene.setCamera({ fov: 50, position: pos, target: target || [0,0,0], aspect: canvas.clientWidth / canvas.clientHeight });
        }

        // Scene 1: Primitives
        function scenePrimitives() {
            clearScene(); cam([0,8,18]);
            addMesh(Mesh.box(0.8,0.8,0.8),     { x:-6, y:1, color:'#e74c3c', name:'box' });
            addMesh(Mesh.sphere(1,24,16),        { x:-3, y:1, color:'#3498db', name:'sphere' });
            addMesh(Mesh.cylinder(0.6,1,20),     { x: 0, y:1, color:'#2ecc71', name:'cylinder' });
            addMesh(Mesh.capsule(0.5,0.6,16,8),  { x: 3, y:1, color:'#f39c12', name:'capsule' });
            addMesh(Mesh.torus(0.8,0.3,24,12),   { x: 6, y:1, color:'#9b59b6', name:'torus' });
            addGround();
            log('Primitives: box, sphere, cylinder, capsule, torus');
        }

        // Scene 2: Transforms
        function sceneTransforms() {
            clearScene(); cam([0,10,20], [0,2,0]);
            var b = Mesh.box(0.5,0.5,0.5);
            addMesh(b.clone(), { x:-6, y:1, color:'#ecf0f1', name:'original' });
            addMesh(b.clone().translate(0,0.5,0),  { x:-3, y:1, color:'#e74c3c', name:'translated' });
            addMesh(b.clone().scale(2,0.5,1),      { x:-0.5, y:1, color:'#3498db', name:'scaled' });
            addMesh(b.clone().rotate(0,1,0,Math.PI/4), { x:2.5, y:1, color:'#2ecc71', name:'rotated' });
            addMesh(Mesh.capsule(0.3,0.5).mirror(0),   { x:5.5, y:1, color:'#f39c12', name:'mirrored' });
            addGround();
            log('Transforms: translate, scale, rotate, mirror');
        }

        // Scene 3: CSG
        function sceneCSG() {
            clearScene(); cam([0,8,16], [0,1,0]);
            var sA = Mesh.sphere(1.2,24,16);
            var sB = Mesh.sphere(1.2,24,16); sB.translate(1,0,0);
            var u = Mesh.union(sA, sB);
            var s = Mesh.subtract(sA, sB);
            var inter = Mesh.intersect(sA, sB);
            addMesh(u,     { x:-3.5, y:1.5, color:'#2ecc71', name:'union' });
            addMesh(s,     { x: 0,   y:1.5, color:'#e74c3c', name:'subtract' });
            addMesh(inter, { x: 3.5, y:1.5, color:'#3498db', name:'intersect' });
            addGround(12);
            log('CSG: union=' + u.triangleCount + 't, subtract=' + s.triangleCount + 't, intersect=' + inter.triangleCount + 't');
        }

        // Scene 4: Simplification
        function sceneSimplify() {
            clearScene(); cam([0,5,16], [0,1,0]);
            var hi = Mesh.sphere(1.5,48,32);
            addMesh(hi.clone(), { x:-5, y:2, color:'#ecf0f1', name:'original' });
            var mid = hi.clone().simplify(0.5);
            addMesh(mid, { x:-1, y:2, color:'#3498db', name:'50%' });
            var lo = hi.clone().simplifyToTriangleCount(100);
            addMesh(lo, { x:3, y:2, color:'#e74c3c', name:'100t' });
            addGround();
            log('Simplify: ' + hi.triangleCount + 't -> 50%=' + mid.triangleCount + 't -> 100t=' + lo.triangleCount + 't');
        }

        // Scene 5: Subdivision
        function sceneSubdivide() {
            clearScene(); cam([0,6,16], [0,1.5,0]);
            var b = Mesh.box(1,1,1);
            addMesh(b.clone(), { x:-5, y:2, color:'#ecf0f1', name:'original' });
            var loop = b.clone().subdivideLoop(2);
            addMesh(loop, { x:-1.5, y:2, color:'#3498db', name:'loop' });
            var cc = b.clone().subdivideCatmullClark(2);
            addMesh(cc, { x:2, y:2, color:'#2ecc71', name:'catmull-clark' });
            addGround();
            log('Subdivide: base=' + b.triangleCount + 't -> loop=' + loop.triangleCount + 't, cc=' + cc.triangleCount + 't');
        }

        // Scene 6: Isosurface
        function sceneIsosurface() {
            clearScene(); cam([0,8,20], [0,2,0]);
            var gs = 32;
            var field = new Float32Array(gs*gs*gs);
            for (var z = 0; z < gs; z++)
                for (var y = 0; y < gs; y++)
                    for (var x = 0; x < gs; x++) {
                        var fx=(x/gs-0.5)*4, fy=(y/gs-0.5)*4, fz=(z/gs-0.5)*4;
                        var d = Math.sqrt(fx*fx+fy*fy+fz*fz) - 1.5;
                        d += Math.sin(fx*3)*0.15 + Math.sin(fy*4)*0.1;
                        field[z*gs*gs+y*gs+x] = -d;
                    }
            var cs = 4.0/gs;
            var mc = Mesh.marchingCubes(field,gs,gs,gs,0,cs); mc.center(); mc.computeNormals();
            var dc = Mesh.dualContour(field,gs,gs,gs,0,cs);   dc.center(); dc.computeNormals();
            addMesh(mc, { x:-4, y:3, color:'#3498db', name:'marching-cubes' });
            addMesh(dc, { x: 4, y:3, color:'#e74c3c', name:'dual-contour' });
            addGround(12);
            log('Isosurface: marchingCubes=' + mc.triangleCount + 't, dualContour=' + dc.triangleCount + 't');
        }

        // Scene 7: Analysis
        function sceneAnalysis() {
            clearScene(); cam([0,8,18], [0,2,0]);
            var sphere = Mesh.sphere(2,24,16);
            var bbox = sphere.computeBBox();
            var vol = sphere.computeVolume();
            var manifold = sphere.isManifold();
            var hit = sphere.raycast([0,0,10],[0,0,-1]);
            addMesh(sphere.clone(), { x:-3, y:2.5, color:'#3498db' });
            if (hit) addMesh(Mesh.sphere(0.1,8,6), { x:-3+hit.position[0], y:2.5+hit.position[1], z:hit.position[2], color:'#e74c3c', emissive:1 });
            var merged = Mesh.merge([Mesh.box(0.5,0.5,0.5), Mesh.sphere(0.3,12,8)]);
            addMesh(merged, { x:3, y:2.5, color:'#f39c12' });
            addGround(12);
            log('vol=' + vol.toFixed(1) + ' manifold=' + manifold + ' rayHit=' + (hit ? 'd='+hit.distance.toFixed(2) : 'miss') + ' merged=' + merged.triangleCount + 't');
        }

        var scenes = [scenePrimitives, sceneTransforms, sceneCSG, sceneSimplify, sceneSubdivide, sceneIsosurface, sceneAnalysis];

        self._handler = function(e) {
            var key = e.key;
            if (key >= '1' && key <= '7') { currentScene = parseInt(key)-1; scenes[currentScene](); }
            else if (key === ' ') paused = !paused;
        };
        document.addEventListener('keydown', self._handler);

        function animate() {
            if (!self.running) return;
            if (!paused) {
                time += 16;
                var t = time * 0.001;
                if (currentScene === 0 || currentScene === 2 || currentScene === 5) {
                    var tgt = currentScene === 0 ? [0,0,0] : [0, currentScene===2?1:2, 0];
                    var r = currentScene === 0 ? 18 : currentScene === 2 ? 16 : 20;
                    cam([Math.sin(t*0.25)*r, 8, Math.cos(t*0.25)*r], tgt);
                }
            }
            requestAnimationFrame(animate);
        }

        scenes[0]();
        animate();
    },
    destroy: function() {
        this.running = false;
        if (this._clearScene) {
            this._clearScene();
            this._clearScene = null;
        }
        if (this._handler) {
            document.removeEventListener('keydown', this._handler);
            this._handler = null;
        }
    }
};
