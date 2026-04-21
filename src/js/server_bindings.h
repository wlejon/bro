#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::engine { class Engine; }

namespace bro::js {

class Worker;

class ServerBindings {
public:
    /// Install bro.server.{tickrate,uptime,stop} backed by the Engine.
    /// Used on the main JSContext for windowed/headless/server modes.
    static void install(JSContext* ctx, engine::Engine* engine);

    /// Install bro.server.{tickrate,uptime,stop} backed by a Worker.
    /// Same JS surface, different lifetime scope: tickrate drives the
    /// worker's own event loop, uptime is measured from when this
    /// worker started, and stop() terminates the worker.
    static void installWorker(JSContext* ctx, Worker* worker);

    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
