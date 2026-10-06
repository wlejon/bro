#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bro::platform::desktop {

/// Requests single-instance lock for the given identifier.
/// If an instance is already running, forwards currentArgs to it and returns false.
/// If this is the first instance, starts IPC listener and returns true.
/// When subsequent instances forward their arguments, onInstanceCallback is invoked.
bool requestSingleInstance(
    const std::string& name,
    const std::vector<std::string>& currentArgs,
    std::function<void(const std::vector<std::string>& args)> onInstanceCallback
);

/// Shuts down single-instance IPC server and releases lock.
void shutdownSingleInstance();

/// Dispatches any pending single-instance forwarded arguments on the main thread.
void pumpSingleInstanceEvents();

/// Test simulation helper: forwards args as if another instance just launched.
bool simulateSingleInstanceMessage(const std::string& name, const std::vector<std::string>& args);

} // namespace bro::platform::desktop
