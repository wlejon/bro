(function(target) {
    return new Proxy(target, {
        get: function(t, prop) {
            if (typeof prop !== 'string') return t[prop];
            var attr = 'data-' + prop.replace(/[A-Z]/g, function(c) {
                return '-' + c.toLowerCase();
            });
            var elId = t.__bro_el_id;
            var elMap = globalThis.__bro_elem_map;
            if (elMap) {
                var wrapper = elMap[String(elId)];
                if (wrapper) return wrapper.getAttribute(attr);
            }
            return t[prop];
        },
        set: function(t, prop, value) {
            if (typeof prop !== 'string') return false;
            var attr = 'data-' + prop.replace(/[A-Z]/g, function(c) {
                return '-' + c.toLowerCase();
            });
            var elId = t.__bro_el_id;
            var elMap = globalThis.__bro_elem_map;
            if (elMap) {
                var wrapper = elMap[String(elId)];
                if (wrapper) wrapper.setAttribute(attr, String(value));
            }
            t[prop] = String(value);
            return true;
        },
        deleteProperty: function(t, prop) {
            if (typeof prop !== 'string') return false;
            var attr = 'data-' + prop.replace(/[A-Z]/g, function(c) {
                return '-' + c.toLowerCase();
            });
            var elId = t.__bro_el_id;
            var elMap = globalThis.__bro_elem_map;
            if (elMap) {
                var wrapper = elMap[String(elId)];
                if (wrapper) wrapper.removeAttribute(attr);
            }
            delete t[prop];
            return true;
        }
    });
})