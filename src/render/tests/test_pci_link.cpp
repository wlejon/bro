#include "render/pci_link.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "  FAIL: " #cond " (line " << __LINE__ << ")" << std::endl;  \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

void testDeviceRanking() {
    std::cout << "[pci_link] test pure device ranking logic" << std::endl;
    using namespace bro::render;

    // 1. Device type score dominance
    {
        DeviceCandidate discrete{1000, 64.0, 8ull * 1024 * 1024 * 1024, 1};
        DeviceCandidate integrated{500, 256.0, 32ull * 1024 * 1024 * 1024, 0};
        CHECK(isDeviceCandidateBetter(discrete, integrated));
        CHECK(!isDeviceCandidateBetter(integrated, discrete));
        CHECK(deviceCandidateSelectionReason(discrete, integrated).find("higher device type score") != std::string::npos);
    }

    // 2. PCIe bandwidth tie-break on equal type scores (the two RTX 4090s scenario)
    {
        // Device 0: x4 link (64 GT/s), index 0
        DeviceCandidate gpu0{1000, 64.0, 24ull * 1024 * 1024 * 1024, 0};
        // Device 1: x16 link (256 GT/s), index 1
        DeviceCandidate gpu1{1000, 256.0, 24ull * 1024 * 1024 * 1024, 1};

        CHECK(isDeviceCandidateBetter(gpu1, gpu0));
        CHECK(!isDeviceCandidateBetter(gpu0, gpu1));
        std::string reason = deviceCandidateSelectionReason(gpu1, gpu0);
        CHECK(reason.find("higher PCIe link bandwidth") != std::string::npos);
        CHECK(reason.find("256 vs 64 GT/s") != std::string::npos);
    }

    // 3. Device-local VRAM tie-break on equal type score and bandwidth
    {
        DeviceCandidate moreVram{1000, 256.0, 24ull * 1024 * 1024 * 1024, 1};
        DeviceCandidate lessVram{1000, 256.0, 16ull * 1024 * 1024 * 1024, 0};
        CHECK(isDeviceCandidateBetter(moreVram, lessVram));
        CHECK(!isDeviceCandidateBetter(lessVram, moreVram));
        std::string reason = deviceCandidateSelectionReason(moreVram, lessVram);
        CHECK(reason.find("more device-local VRAM") != std::string::npos);
    }

    // 4. Enumeration order tie-break when score, bandwidth, and VRAM are all equal
    {
        DeviceCandidate dev0{1000, 256.0, 24ull * 1024 * 1024 * 1024, 0};
        DeviceCandidate dev1{1000, 256.0, 24ull * 1024 * 1024 * 1024, 1};
        CHECK(isDeviceCandidateBetter(dev0, dev1));
        CHECK(!isDeviceCandidateBetter(dev1, dev0));
        std::string reason = deviceCandidateSelectionReason(dev0, dev1);
        CHECK(reason.find("enumeration order") != std::string::npos);
    }

    // 5. Array sorting with strict weak ordering
    {
        std::vector<DeviceCandidate> list = {
            {500, 128.0, 8ull * 1024 * 1024 * 1024, 2},   // Integrated
            {1000, 64.0, 24ull * 1024 * 1024 * 1024, 0},  // RTX 4090 @ x4
            {1000, 256.0, 24ull * 1024 * 1024 * 1024, 1}, // RTX 4090 @ x16 (winner)
            {1000, 256.0, 16ull * 1024 * 1024 * 1024, 3}, // Fast but less VRAM
            {100, 32.0, 4ull * 1024 * 1024 * 1024, 4},    // CPU / Other
        };

        std::sort(list.begin(), list.end(), isDeviceCandidateBetter);

        CHECK(list[0].index == 1); // Best: RTX 4090 @ x16 (score 1000, bw 256, 24GB)
        CHECK(list[1].index == 3); // Second: score 1000, bw 256, 16GB (higher bw than x4)
        CHECK(list[2].index == 0); // Third: RTX 4090 @ x4 (score 1000, bw 64, 24GB)
        CHECK(list[3].index == 2); // Fourth: integrated
        CHECK(list[4].index == 4); // Fifth: cpu/other
    }
}

void testLinuxSpeedParsing() {
    std::cout << "[pci_link] test Linux sysfs speed string parsing" << std::endl;
    using namespace bro::render;

    // Standard sysfs speed strings
    CHECK(std::abs(parseLinuxPciSpeed("16.0 GT/s PCIe") - 16.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("16.0 GT/s PCIe\n") - 16.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("2.5 GT/s PCIe") - 2.5) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("5.0 GT/s PCIe") - 5.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("8.0 GT/s PCIe") - 8.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("32.0 GT/s PCIe") - 32.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("64.0 GT/s PCIe") - 64.0) < 1e-6);

    // Variants without "PCIe" or extra spaces
    CHECK(std::abs(parseLinuxPciSpeed("16 GT/s") - 16.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("  8.0 GT/s \r\n") - 8.0) < 1e-6);
    CHECK(std::abs(parseLinuxPciSpeed("2.5 gt/s") - 2.5) < 1e-6);

    // Invalid / empty / unrecognized strings
    CHECK(parseLinuxPciSpeed("Unknown") == 0.0);
    CHECK(parseLinuxPciSpeed("") == 0.0);
    CHECK(parseLinuxPciSpeed("   ") == 0.0);
    CHECK(parseLinuxPciSpeed("16.0 Gbps") == 0.0);
    CHECK(parseLinuxPciSpeed("16.0") == 0.0);
    CHECK(parseLinuxPciSpeed("invalid") == 0.0);
    CHECK(parseLinuxPciSpeed("-5.0 GT/s") == 0.0);
}

void testPciLinkQuerySmoke() {
    std::cout << "[pci_link] test live query safety" << std::endl;
    // An invalid PCI address should safely return a zero link
    bro::render::PciLink link = bro::render::queryPciLink(0xffff, 0xff, 0x1f, 0x7);
    CHECK(link.width >= 0);
    CHECK(link.gtPerSec >= 0.0);
    CHECK(link.bandwidth() >= 0.0);
}

} // namespace

int main() {
    std::cout << "=== Running test_pci_link ===" << std::endl;
    testDeviceRanking();
    testLinuxSpeedParsing();
    testPciLinkQuerySmoke();

    if (gFailures > 0) {
        std::cerr << "FAILED: " << gFailures << " check(s)" << std::endl;
        return 1;
    }
    std::cout << "test_pci_link: all checks passed" << std::endl;
    return 0;
}
