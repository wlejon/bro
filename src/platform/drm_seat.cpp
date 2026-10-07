#include "platform/drm_seat.h"

#include <chrono>
#include <thread>

namespace bro::platform {

DrmSeatPlatform::DrmSeatPlatform() = default;

DrmSeatPlatform::~DrmSeatPlatform() {
#if defined(__linux__) && BRO_WITH_SEAT
    activeLock_.reset();
    openDevicesByFd_.clear();
    seat_.reset();
    sessionMgr_.reset();
    inhibitorMgr_.reset();
#endif
}

bool DrmSeatPlatform::initSeat(const std::string& seatName) {
#if defined(__linux__) && BRO_WITH_SEAT
    broseat::SeatConfig cfg;
    cfg.seat_name = seatName;
    std::string err;
    seat_ = broseat::Seat::create(cfg, &err);
    if (seat_) {
        for (int i = 0; i < 20 && !seat_->is_active(); ++i) {
            seat_->dispatch(50);
            if (seat_->is_active()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        seatActive_ = seat_->is_active();
    } else {
        seatActive_ = false;
    }

    sessionMgr_ = broseat::SessionManager::create(&err);
    sessionActive_ = (sessionMgr_ != nullptr);

    inhibitorMgr_ = broseat::InhibitManager::create(&err);

    return seat_ != nullptr;
#else
    (void)seatName;
    return false;
#endif
}

int DrmSeatPlatform::openDevice(const std::string& path) {
#if defined(__linux__) && BRO_WITH_SEAT
    if (!seat_) return -1;
    std::string err;
    auto dev = seat_->open_device(path, &err);
    if (!dev || !dev->is_valid()) return -1;
    int fd = dev->fd();
    openDevicesByFd_[fd] = std::move(dev);
    return fd;
#else
    (void)path;
    return -1;
#endif
}

void DrmSeatPlatform::closeDevice(int fd) {
#if defined(__linux__) && BRO_WITH_SEAT
    auto it = openDevicesByFd_.find(fd);
    if (it != openDevicesByFd_.end()) {
        it->second->close();
        openDevicesByFd_.erase(it);
    }
#else
    (void)fd;
#endif
}

bool DrmSeatPlatform::switchVT(int vtNumber) {
#if defined(__linux__) && BRO_WITH_SEAT
    if (!seat_) return false;
    return seat_->switch_vt(vtNumber) == 0;
#else
    (void)vtNumber;
    return false;
#endif
}

bool DrmSeatPlatform::startGraphicalSession() {
#if defined(__linux__) && BRO_WITH_SEAT
    return broseat::start_unit(std::string(broseat::kGraphicalSessionTarget));
#else
    return false;
#endif
}

bool DrmSeatPlatform::stopGraphicalSession() {
#if defined(__linux__) && BRO_WITH_SEAT
    return broseat::stop_unit(std::string(broseat::kGraphicalSessionTarget));
#else
    return false;
#endif
}

bool DrmSeatPlatform::isGraphicalSessionActive() const {
#if defined(__linux__) && BRO_WITH_SEAT
    return broseat::is_unit_active(std::string(broseat::kGraphicalSessionTarget));
#else
    return false;
#endif
}

bool DrmSeatPlatform::importEnvironment(const std::vector<std::string>& envVars) {
#if defined(__linux__) && BRO_WITH_SEAT
    return broseat::export_environment(envVars);
#else
    (void)envVars;
    return false;
#endif
}

bool DrmSeatPlatform::hasSessionManager() const {
#if defined(__linux__) && BRO_WITH_SEAT
    return sessionMgr_ != nullptr;
#else
    return false;
#endif
}

bool DrmSeatPlatform::hasInhibitManager() const {
#if defined(__linux__) && BRO_WITH_SEAT
    return inhibitorMgr_ != nullptr;
#else
    return false;
#endif
}

bool DrmSeatPlatform::setIdleInhibited(bool inhibit, const std::string& reason) {
#if defined(__linux__) && BRO_WITH_SEAT
    if (!inhibitorMgr_) return false;
    if (inhibit) {
        activeLock_ = inhibitorMgr_->inhibit("idle:sleep", "bro", reason);
        idleInhibited_ = (activeLock_ != nullptr && activeLock_->is_held());
        return idleInhibited_;
    } else {
        activeLock_.reset();
        idleInhibited_ = false;
        return true;
    }
#else
    (void)inhibit;
    (void)reason;
    return false;
#endif
}

bool DrmSeatPlatform::isIdleInhibited() const {
    return idleInhibited_;
}

bool DrmSeatPlatform::isSeatActive() const {
#if defined(__linux__) && BRO_WITH_SEAT
    return seat_ && seat_->is_active();
#else
    return false;
#endif
}

void DrmSeatPlatform::pollEvents() {
#if defined(__linux__) && BRO_WITH_SEAT
    if (seat_) {
        seat_->dispatch(0);
        bool currentActive = seat_->is_active();
        if (currentActive != seatActive_) {
            seatActive_ = currentActive;
            if (activeCallback_) activeCallback_(seatActive_);
        }
    }
#endif
}

} // namespace bro::platform
