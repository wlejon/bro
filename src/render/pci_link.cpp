#include "render/pci_link.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <pciprop.h>

namespace {

double pciSpeedEnumToGtPerSec(uint32_t speedEnum) {
    switch (speedEnum) {
    case 1: return 2.5;
    case 2: return 5.0;
    case 3: return 8.0;
    case 4: return 16.0;
    case 5: return 32.0;
    case 6: return 64.0;
    default: return 0.0;
    }
}

} // namespace
#endif

#if defined(__linux__)
namespace {

int readLinuxSysfsInt(const char* dir, const char* file) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    char buf[64];
    if (!std::fgets(buf, sizeof(buf), f)) {
        std::fclose(f);
        return 0;
    }
    std::fclose(f);
    char* end = nullptr;
    long val = std::strtol(buf, &end, 10);
    return (val > 0 && end != buf) ? static_cast<int>(val) : 0;
}

double readLinuxSysfsSpeed(const char* dir, const char* file) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE* f = std::fopen(path, "r");
    if (!f) return 0.0;
    char buf[128];
    if (!std::fgets(buf, sizeof(buf), f)) {
        std::fclose(f);
        return 0.0;
    }
    std::fclose(f);
    return bro::render::parseLinuxPciSpeed(buf);
}

} // namespace
#endif

namespace bro::render {

double parseLinuxPciSpeed(std::string_view str) {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t' ||
                            str.front() == '\n' || str.front() == '\r')) {
        str.remove_prefix(1);
    }
    if (str.empty()) return 0.0;

    std::string s(str);
    char* end = nullptr;
    double val = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || val <= 0.0) return 0.0;

    while (*end == ' ' || *end == '\t') ++end;
    if ((end[0] == 'G' || end[0] == 'g') &&
        (end[1] == 'T' || end[1] == 't') &&
        end[2] == '/' &&
        (end[3] == 'S' || end[3] == 's')) {
        return val;
    }
    return 0.0;
}

PciLink queryPciLink(uint32_t domain, uint32_t bus, uint32_t device, uint32_t function) {
#if defined(_WIN32)
    (void)domain;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return {};
    }

    PciLink result{};
    const uint32_t targetAddress = (device << 16) | function;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); ++i) {
        DEVPROPTYPE propType = 0;
        uint32_t devBus = 0;
        DWORD requiredSize = 0;
        if (!SetupDiGetDevicePropertyW(devInfo, &devData, &DEVPKEY_Device_BusNumber,
                                      &propType, reinterpret_cast<PBYTE>(&devBus),
                                      sizeof(devBus), &requiredSize, 0)) {
            continue;
        }

        uint32_t devAddr = 0;
        if (!SetupDiGetDevicePropertyW(devInfo, &devData, &DEVPKEY_Device_Address,
                                      &propType, reinterpret_cast<PBYTE>(&devAddr),
                                      sizeof(devAddr), &requiredSize, 0)) {
            continue;
        }

        if (devBus == bus && devAddr == targetAddress) {
            uint32_t curWidth = 0;
            SetupDiGetDevicePropertyW(devInfo, &devData, &DEVPKEY_PciDevice_CurrentLinkWidth,
                                      &propType, reinterpret_cast<PBYTE>(&curWidth),
                                      sizeof(curWidth), &requiredSize, 0);

            uint32_t curSpeedEnum = 0;
            SetupDiGetDevicePropertyW(devInfo, &devData, &DEVPKEY_PciDevice_CurrentLinkSpeed,
                                      &propType, reinterpret_cast<PBYTE>(&curSpeedEnum),
                                      sizeof(curSpeedEnum), &requiredSize, 0);

            uint32_t maxWidth = 0;
            SetupDiGetDevicePropertyW(devInfo, &devData, &DEVPKEY_PciDevice_MaxLinkWidth,
                                      &propType, reinterpret_cast<PBYTE>(&maxWidth),
                                      sizeof(maxWidth), &requiredSize, 0);

            uint32_t maxSpeedEnum = 0;
            SetupDiGetDevicePropertyW(devInfo, &devData, &DEVPKEY_PciDevice_MaxLinkSpeed,
                                      &propType, reinterpret_cast<PBYTE>(&maxSpeedEnum),
                                      sizeof(maxSpeedEnum), &requiredSize, 0);

            result.currentWidth = static_cast<int>(curWidth);
            result.currentGtPerSec = pciSpeedEnumToGtPerSec(curSpeedEnum);
            result.maxWidth = static_cast<int>(maxWidth);
            result.maxGtPerSec = pciSpeedEnumToGtPerSec(maxSpeedEnum);

            result.width = (result.currentWidth > 0) ? result.currentWidth : result.maxWidth;
            result.gtPerSec = (result.maxGtPerSec > 0.0) ? result.maxGtPerSec : result.currentGtPerSec;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;

#elif defined(__linux__)
    char dir[256];
    std::snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%04x:%02x:%02x.%x",
                  domain, bus, device, function);
    PciLink link;
    link.currentWidth = readLinuxSysfsInt(dir, "current_link_width");
    link.currentGtPerSec = readLinuxSysfsSpeed(dir, "current_link_speed");
    link.maxWidth = readLinuxSysfsInt(dir, "max_link_width");
    link.maxGtPerSec = readLinuxSysfsSpeed(dir, "max_link_speed");

    link.width = (link.currentWidth > 0) ? link.currentWidth : link.maxWidth;
    link.gtPerSec = (link.maxGtPerSec > 0.0) ? link.maxGtPerSec : link.currentGtPerSec;
    return link;

#else
    (void)domain;
    (void)bus;
    (void)device;
    (void)function;
    return {};
#endif
}

bool isDeviceCandidateBetter(const DeviceCandidate& a, const DeviceCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    if (std::abs(a.bandwidth - b.bandwidth) > 1e-6) return a.bandwidth > b.bandwidth;
    if (a.vram != b.vram) return a.vram > b.vram;
    return a.index < b.index;
}

std::string deviceCandidateSelectionReason(const DeviceCandidate& winner, const DeviceCandidate& runnerUp) {
    if (winner.score != runnerUp.score) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "higher device type score (%d vs %d)",
                      winner.score, runnerUp.score);
        return buf;
    }
    if (std::abs(winner.bandwidth - runnerUp.bandwidth) > 1e-6) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "higher PCIe link bandwidth (%g vs %g GT/s)",
                      winner.bandwidth, runnerUp.bandwidth);
        return buf;
    }
    if (winner.vram != runnerUp.vram) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "more device-local VRAM (%llu vs %llu MB)",
                      static_cast<unsigned long long>(winner.vram / (1024 * 1024)),
                      static_cast<unsigned long long>(runnerUp.vram / (1024 * 1024)));
        return buf;
    }
    return "enumeration order";
}

} // namespace bro::render
