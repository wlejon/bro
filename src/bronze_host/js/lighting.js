// lighting.js — the public shape of bro.lighting.*, assembled over the
// natives under __bro_native.lighting.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_F64 = new Float64Array(0);
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    const toF64 = (v) => v instanceof Float64Array ? v : Float64Array.from(v);

    // ---- bro.lighting.LightNode ----------------------------------------------
    function LightNode() {
        throw new TypeError("bro.lighting.LightNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.LightNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.LightNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, LightNode.prototype);
    }
    fn(mount(bro, "lighting"), "LightNode", LightNode);
    accessor(LightNode.prototype, "color",
        function () {
            return Array.from(__bro_native.lighting.LightNode_color_get(this));
        },
        function (v) {
            __bro_native.lighting.LightNode_color_set(this, toF64(v));
        });
    accessor(LightNode.prototype, "intensity",
        function () {
            return __bro_native.lighting.LightNode_intensity_get(this);
        },
        function (v) {
            __bro_native.lighting.LightNode_intensity_set(this, v);
        });
    accessor(LightNode.prototype, "range",
        function () {
            return __bro_native.lighting.LightNode_range_get(this);
        },
        function (v) {
            __bro_native.lighting.LightNode_range_set(this, v);
        });
    accessor(LightNode.prototype, "innerCone",
        function () {
            return __bro_native.lighting.LightNode_innerCone_get(this);
        },
        function (v) {
            __bro_native.lighting.LightNode_innerCone_set(this, v);
        });
    accessor(LightNode.prototype, "outerCone",
        function () {
            return __bro_native.lighting.LightNode_outerCone_get(this);
        },
        function (v) {
            __bro_native.lighting.LightNode_outerCone_set(this, v);
        });
    accessor(LightNode.prototype, "castShadow",
        function () {
            return __bro_native.lighting.LightNode_castShadow_get(this);
        },
        function (v) {
            __bro_native.lighting.LightNode_castShadow_set(this, v);
        });

    // ---- bro.lighting.ShapeNode ----------------------------------------------
    function ShapeNode() {
        throw new TypeError("bro.lighting.ShapeNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.ShapeNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.ShapeNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, ShapeNode.prototype);
    }
    fn(mount(bro, "lighting"), "ShapeNode", ShapeNode);
    accessor(ShapeNode.prototype, "shapeType",
        function () {
            return __bro_native.lighting.ShapeNode_shapeType_get(this);
        },
        function (v) {
            __bro_native.lighting.ShapeNode_shapeType_set(this, v);
        });
    accessor(ShapeNode.prototype, "color",
        function () {
            return Array.from(__bro_native.lighting.ShapeNode_color_get(this));
        },
        function (v) {
            __bro_native.lighting.ShapeNode_color_set(this, toF64(v));
        });
    accessor(ShapeNode.prototype, "size",
        function () {
            return Array.from(__bro_native.lighting.ShapeNode_size_get(this));
        },
        function (v) {
            __bro_native.lighting.ShapeNode_size_set(this, toF64(v));
        });

    // ---- bro.lighting.SpriteNode ---------------------------------------------
    function SpriteNode() {
        throw new TypeError("bro.lighting.SpriteNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.SpriteNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.SpriteNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, SpriteNode.prototype);
    }
    fn(mount(bro, "lighting"), "SpriteNode", SpriteNode);
    accessor(SpriteNode.prototype, "texture",
        function () {
            return __bro_native.lighting.SpriteNode_texture_get(this);
        },
        function (v) {
            __bro_native.lighting.SpriteNode_texture_set(this, v);
        });
    accessor(SpriteNode.prototype, "size",
        function () {
            return Array.from(__bro_native.lighting.SpriteNode_size_get(this));
        },
        function (v) {
            __bro_native.lighting.SpriteNode_size_set(this, toF64(v));
        });
    fn(SpriteNode.prototype, "addAnimation", function addAnimation(name, animSpec) {
        return this;
    });
    fn(SpriteNode.prototype, "play", function play(name) {
        if (name === undefined) throw new TypeError("bro.lighting.SpriteNode.prototype.play: name is required");
        __bro_native.lighting.SpriteNode_play(this, name);
    });
    fn(SpriteNode.prototype, "stop", function stop() {
        __bro_native.lighting.SpriteNode_stop(this);
    });

    // ---- bro.lighting.HtmlNode -----------------------------------------------
    function HtmlNode() {
        throw new TypeError("bro.lighting.HtmlNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.HtmlNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.HtmlNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, HtmlNode.prototype);
    }
    fn(mount(bro, "lighting"), "HtmlNode", HtmlNode);
    fn(HtmlNode.prototype, "setHtml", function setHtml(html) {
        if (html === undefined) throw new TypeError("bro.lighting.HtmlNode.prototype.setHtml: html is required");
        __bro_native.lighting.HtmlNode_setHtml(this, html);
    });
    fn(HtmlNode.prototype, "markHtmlDirty", function markHtmlDirty() {
        __bro_native.lighting.HtmlNode_markHtmlDirty(this);
    });

    // ---- bro.lighting.ParticleNode -------------------------------------------
    function ParticleNode() {
        throw new TypeError("bro.lighting.ParticleNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.ParticleNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.ParticleNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, ParticleNode.prototype);
    }
    fn(mount(bro, "lighting"), "ParticleNode", ParticleNode);
    fn(ParticleNode.prototype, "burst", function burst(count) {
        if (count === undefined) throw new TypeError("bro.lighting.ParticleNode.prototype.burst: count is required");
        __bro_native.lighting.ParticleNode_burst(this, count);
    });
    fn(ParticleNode.prototype, "clear", function clear() {
        __bro_native.lighting.ParticleNode_clear(this);
    });

    // ---- bro.lighting.Particles3DNode ----------------------------------------
    function Particles3DNode() {
        throw new TypeError("bro.lighting.Particles3DNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.Particles3DNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.Particles3DNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, Particles3DNode.prototype);
    }
    fn(mount(bro, "lighting"), "Particles3DNode", Particles3DNode);
    fn(Particles3DNode.prototype, "burst", function burst(count) {
        if (count === undefined) throw new TypeError("bro.lighting.Particles3DNode.prototype.burst: count is required");
        __bro_native.lighting.Particles3DNode_burst(this, count);
    });
    fn(Particles3DNode.prototype, "clear", function clear() {
        __bro_native.lighting.Particles3DNode_clear(this);
    });

    // ---- bro.lighting.GaussianSplatNode --------------------------------------
    function GaussianSplatNode() {
        throw new TypeError("bro.lighting.GaussianSplatNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.GaussianSplatNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.GaussianSplatNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, GaussianSplatNode.prototype);
    }
    fn(mount(bro, "lighting"), "GaussianSplatNode", GaussianSplatNode);
    fn(GaussianSplatNode.prototype, "savePly", function savePly(path) {
        if (path === undefined) throw new TypeError("bro.lighting.GaussianSplatNode.prototype.savePly: path is required");
        __bro_native.lighting.GaussianSplatNode_savePly(this, path);
    });

    // ---- bro.lighting.DecalNode ----------------------------------------------
    function DecalNode() {
        throw new TypeError("bro.lighting.DecalNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.DecalNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.DecalNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, DecalNode.prototype);
    }
    fn(mount(bro, "lighting"), "DecalNode", DecalNode);
    accessor(DecalNode.prototype, "texture",
        function () {
            return __bro_native.lighting.DecalNode_texture_get(this);
        },
        function (v) {
            __bro_native.lighting.DecalNode_texture_set(this, v);
        });
    accessor(DecalNode.prototype, "size",
        function () {
            return Array.from(__bro_native.lighting.DecalNode_size_get(this));
        },
        function (v) {
            __bro_native.lighting.DecalNode_size_set(this, toF64(v));
        });

    // ---- bro.lighting.ReflectionProbeNode ------------------------------------
    function ReflectionProbeNode() {
        throw new TypeError("bro.lighting.ReflectionProbeNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.lighting.ReflectionProbeNodeProto;
        if (proto === undefined) throw new Error("bro.lighting.ReflectionProbeNode: native class prototype not published (registerNatives_lighting did not run)");
        Object.setPrototypeOf(proto, ReflectionProbeNode.prototype);
    }
    fn(mount(bro, "lighting"), "ReflectionProbeNode", ReflectionProbeNode);
    fn(ReflectionProbeNode.prototype, "probeCapture", function probeCapture() {
        __bro_native.lighting.ReflectionProbeNode_probeCapture(this);
    });
})();
