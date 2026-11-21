#pragma once

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Register the `voxeldag` brokernel module and mount `bro.voxeldag` onto `bro`.
void installVoxelDag(engine::Engine& engine);

}  // namespace bro::bronze_host
