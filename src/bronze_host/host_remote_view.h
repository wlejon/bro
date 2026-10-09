#pragma once
// <remoteview>: a bro.remote.connect() session's screen as an element
// (BRO_WITH_REMOTE). host_remote_view.cpp is the engine's RemoteViewHost (the
// pictures, the input) and HTMLRemoteViewElement's members; docs/remote-api.js
// is the contract.

#include "bronze_host/host_builder.h"

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

// Once per process (installRemoteHost): broremote's viewer hooks, the
// engine's RemoteViewHost and the frame pump.
void installRemoteViewHost(engine::Engine& engine);

// HTMLRemoteViewElement's prototype members.
void decorateRemoteViewProto(ObjectBuilder& b);

}  // namespace bro::bronze_host
