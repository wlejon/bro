#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__) && defined(BRO_WITH_SEAT)
#include <broseat/broseat.h>
#include <broseat/session.h>
#include <broseat/inhibit.h>
#endif

namespace bro::platform {

class DrmSeatPlatform {
public:
    DrmSeatPlatform();
    ~DrmSeatPlatform();

    DrmSeatPlatform(const DrmSeatPlatform&) = delete;
    DrmSeatPlatform& operator=(const DrmSeatPlatform&) = delete;

    /// Initialize seat management on the given seat (e.g. "seat0")
    bool initSeat(const std::string& seatName = "seat0");

    /// Open a device node (e.g. /dev/dri/card0 or /dev/input/event0)
    int openDevice(const std::string& path);
    void closeDevice(int fd);

    /// Switch virtual terminal
    bool switchVT(int vtNumber);

    /// Systemd graphical-session.target integration
    bool startGraphicalSession();
    bool stopGraphicalSession();
    bool importEnvironment(const std::vector<std::string>& envVars);
    bool isGraphicalSessionActive() const;

    /// Idle and sleep inhibitor
    bool setIdleInhibited(bool inhibit, const std::string& reason = "bro desktop active");
    bool isIdleInhibited() const;

    bool isSeatActive() const;
    bool isSessionActive() const { return sessionActive_; }
    bool hasSessionManager() const;
    bool hasInhibitManager() const;

    /// Set callback when seat / VT active status changes
    void setActiveChangeCallback(std::function<void(bool active)> cb) { activeCallback_ = std::move(cb); }

    /// Poll pending seat and session events
    void pollEvents();

#if defined(__linux__) && defined(BRO_WITH_SEAT)
    broseat::Seat* seat() { return seat_.get(); }
    const broseat::Seat* seat() const { return seat_.get(); }
    broseat::SessionManager* sessionManager() { return sessionMgr_.get(); }
    const broseat::SessionManager* sessionManager() const { return sessionMgr_.get(); }
    broseat::InhibitManager* inhibitorManager() { return inhibitorMgr_.get(); }
    const broseat::InhibitManager* inhibitorManager() const { return inhibitorMgr_.get(); }
#endif

private:
#if defined(__linux__) && defined(BRO_WITH_SEAT)
    std::unique_ptr<broseat::Seat> seat_;
    std::unique_ptr<broseat::SessionManager> sessionMgr_;
    std::unique_ptr<broseat::InhibitManager> inhibitorMgr_;
    std::unique_ptr<broseat::InhibitorLock> activeLock_;
    std::unordered_map<int, std::unique_ptr<broseat::SeatDevice>> openDevicesByFd_;
#endif
    bool seatActive_ = false;
    bool sessionActive_ = false;
    bool idleInhibited_ = false;
    std::function<void(bool active)> activeCallback_;
};

} // namespace bro::platform
