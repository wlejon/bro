// animation.js — the public shape of bro.animation.Tween, bro.animation.AnimationPlayer, assembled over the
// natives under __bro_native.animation.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    // ---- bro.animation.Tween -------------------------------------------------
    function Tween() {
        throw new TypeError("bro.animation.Tween is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.animation.TweenProto;
        if (proto === undefined) throw new Error("bro.animation.Tween: native class prototype not published (registerNatives_animation did not run)");
        Object.setPrototypeOf(proto, Tween.prototype);
    }
    fn(mount(bro, "animation"), "Tween", Tween);
    globalThis.Tween = Tween;

    fn(Tween.prototype, "to", function to(target, props, duration, opts) {
        if (duration === undefined) throw new TypeError("bro.animation.Tween.prototype.to: duration is required");
        let onUpdate = 0;
        if (opts && typeof opts.onUpdate === 'function') onUpdate = opts.onUpdate;
        const targetId = (target && typeof target.id === 'number') ? target.id : 0;
        __bro_native.animation.Tween_to(this, targetId, props || {}, +duration, opts || {}, onUpdate);
        return this;
    });
    fn(Tween.prototype, "parallel", function parallel() {
        __bro_native.animation.Tween_parallel(this);
        return this;
    });
    fn(Tween.prototype, "call", function call(callback) {
        if (callback === undefined) throw new TypeError("bro.animation.Tween.prototype.call: callback is required");
        return __bro_native.animation.Tween_call(this, callback);
    });
    fn(Tween.prototype, "loop", function loop(count) {
        return __bro_native.animation.Tween_loop(this, count !== undefined, count === undefined ? 0 : count);
    });
    fn(Tween.prototype, "start", function start() {
        return __bro_native.animation.Tween_start(this);
    });
    fn(Tween.prototype, "stop", function stop() {
        return __bro_native.animation.Tween_stop(this);
    });
    fn(Tween.prototype, "pause", function pause() {
        return __bro_native.animation.Tween_pause(this);
    });
    fn(Tween.prototype, "resume", function resume() {
        return __bro_native.animation.Tween_resume(this);
    });
    fn(Tween.prototype, "destroy", function destroy() {
        __bro_native.animation.Tween_destroy(this);
    });
    accessor(Tween.prototype, "isRunning",
        function () {
            return __bro_native.animation.Tween_isRunning(this);
        },
        undefined);
    accessor(Tween.prototype, "isPaused",
        function () {
            return __bro_native.animation.Tween_isPaused(this);
        },
        undefined);
    accessor(Tween.prototype, "isFinished",
        function () {
            return __bro_native.animation.Tween_isFinished(this);
        },
        undefined);
    accessor(Tween.prototype, "onFinished",
        function () {
            return undefined;
        },
        function (cb) {
            __bro_native.animation.Tween_setOnFinished(this, cb);
        });

    // ---- bro.animation.AnimationPlayer ---------------------------------------
    function AnimationPlayer() {
        throw new TypeError("bro.animation.AnimationPlayer is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.animation.AnimationPlayerProto;
        if (proto === undefined) throw new Error("bro.animation.AnimationPlayer: native class prototype not published (registerNatives_animation did not run)");
        Object.setPrototypeOf(proto, AnimationPlayer.prototype);
    }
    fn(mount(bro, "animation"), "AnimationPlayer", AnimationPlayer);
    globalThis.AnimationPlayer = AnimationPlayer;

    fn(AnimationPlayer.prototype, "addClip", function addClip(name, clip) {
        if (name === undefined) throw new TypeError("bro.animation.AnimationPlayer.prototype.addClip: name is required");
        if (clip === undefined) throw new TypeError("bro.animation.AnimationPlayer.prototype.addClip: clip is required");
        const err = __bro_native.animation.AnimationPlayer_addClip(this, name, clip);
        if (err) throw new TypeError("addClip: " + err);
        return this;
    });
    fn(AnimationPlayer.prototype, "clipDef", function clipDef(name) {
        if (name === undefined) throw new TypeError("bro.animation.AnimationPlayer.prototype.clipDef: name is required");
        const json = __bro_native.animation.AnimationPlayer_clipDef(this, name);
        return json ? JSON.parse(json) : null;
    });
    fn(AnimationPlayer.prototype, "play", function play(clipName, opts) {
        if (clipName === undefined) throw new TypeError("bro.animation.AnimationPlayer.prototype.play: clipName is required");
        const err = __bro_native.animation.AnimationPlayer_play(this, clipName, opts || {});
        if (err) throw new TypeError("play: " + err);
        return this;
    });
    fn(AnimationPlayer.prototype, "pause", function pause() {
        __bro_native.animation.AnimationPlayer_pause(this);
    });
    fn(AnimationPlayer.prototype, "resume", function resume() {
        __bro_native.animation.AnimationPlayer_resume(this);
    });
    fn(AnimationPlayer.prototype, "stop", function stop() {
        __bro_native.animation.AnimationPlayer_stop(this);
    });
    fn(AnimationPlayer.prototype, "seek", function seek(time) {
        if (time === undefined) throw new TypeError("bro.animation.AnimationPlayer.prototype.seek: time is required");
        __bro_native.animation.AnimationPlayer_seek(this, time);
    });
    fn(AnimationPlayer.prototype, "destroy", function destroy() {
        __bro_native.animation.AnimationPlayer_destroy(this);
    });
    accessor(AnimationPlayer.prototype, "playing",
        function () {
            return __bro_native.animation.AnimationPlayer_playing(this);
        },
        undefined);
    accessor(AnimationPlayer.prototype, "currentClip",
        function () {
            return __bro_native.animation.AnimationPlayer_currentClip(this);
        },
        undefined);
    accessor(AnimationPlayer.prototype, "currentTime",
        function () {
            return __bro_native.animation.AnimationPlayer_currentTime(this);
        },
        undefined);
    accessor(AnimationPlayer.prototype, "speed",
        function () {
            return __bro_native.animation.AnimationPlayer_speed_get(this);
        },
        function (v) {
            __bro_native.animation.AnimationPlayer_speed_set(this, +v);
        });
    accessor(AnimationPlayer.prototype, "onFinished",
        function () {
            return undefined;
        },
        function (cb) {
            __bro_native.animation.AnimationPlayer_setOnFinished(this, cb);
        });
    accessor(AnimationPlayer.prototype, "onEvent",
        function () {
            return undefined;
        },
        function (cb) {
            __bro_native.animation.AnimationPlayer_setOnEvent(this, cb);
        });
})();
