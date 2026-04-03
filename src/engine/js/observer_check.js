(function() {
    if (globalThis.__bro_resize_observers) {
        for (var i = 0; i < globalThis.__bro_resize_observers.length; i++)
            globalThis.__bro_resize_observers[i]._check();
    }
    if (globalThis.__bro_intersection_observers) {
        for (var i = 0; i < globalThis.__bro_intersection_observers.length; i++)
            globalThis.__bro_intersection_observers[i]._check();
    }
})();
