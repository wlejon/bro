#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bro::render {

struct PciLink {
    int width = 0;          // Effective link width (negotiated slot width, or max fallback)
    double gtPerSec = 0.0;  // Effective link speed in GT/s (max speed, or current fallback)

    int currentWidth = 0;
    double currentGtPerSec = 0.0;
    int maxWidth = 0;
    double maxGtPerSec = 0.0;

    double bandwidth() const { return width * gtPerSec; }
};

/// Query PCIe link width and speed for the given PCI device address.
/// Returns a zero PciLink on failure or unsupported platform.
PciLink queryPciLink(uint32_t domain, uint32_t bus, uint32_t device, uint32_t function);

/// Parse a Linux sysfs speed string (e.g. "16.0 GT/s PCIe") into GT/s.
/// Returns 0.0 on failure or unrecognized format.
double parseLinuxPciSpeed(std::string_view str);

/// Device candidate for ranking during physical device selection.
struct DeviceCandidate {
    int score = 0;
    double bandwidth = 0.0;
    uint64_t vram = 0;
    uint32_t index = 0;
};

/// Evaluates if candidate `a` is strictly preferred over candidate `b`:
/// 1. Type score (higher is better)
/// 2. PCIe link bandwidth (higher is better)
/// 3. Device-local VRAM (higher is better)
/// 4. Enumeration order (lower index is better)
bool isDeviceCandidateBetter(const DeviceCandidate& a, const DeviceCandidate& b);

/// Returns a human-readable explanation of why `winner` was preferred over `runnerUp`.
std::string deviceCandidateSelectionReason(const DeviceCandidate& winner, const DeviceCandidate& runnerUp);

} // namespace bro::render
