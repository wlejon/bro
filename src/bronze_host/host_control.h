#pragma once

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Registers the agent-control commands that need the JS realm (eval, dom,
/// inspect, style) on the engine's ControlServer, and the headless test
/// globals controlCommand / controlResult. host_control.cpp.
void installControlCommands(engine::Engine& engine);

}  // namespace bro::bronze_host
