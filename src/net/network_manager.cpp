#include "net/network_manager.h"
#include "util/log.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <cassert>
#include <cstring>

namespace bro::net {

// GNS uses a global callback — route it to the active NetworkManager instance.
static NetworkManager* g_instance = nullptr;

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() {
    shutdown();
}

bool NetworkManager::init() {
    if (initialized_) return true;

    SteamNetworkingErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        LOG_ERROR("[net] GameNetworkingSockets_Init failed: %s", errMsg);
        return false;
    }

    sockets_ = SteamNetworkingSockets();
    if (!sockets_) {
        LOG_ERROR("[net] Failed to get ISteamNetworkingSockets interface");
        GameNetworkingSockets_Kill();
        return false;
    }

    // Set up debug output
    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Msg,
        [](ESteamNetworkingSocketsDebugOutputType type, const char* msg) {
            if (type == k_ESteamNetworkingSocketsDebugOutputType_Bug ||
                type == k_ESteamNetworkingSocketsDebugOutputType_Error) {
                LOG_ERROR("[net/gns] %s", msg);
            } else if (type == k_ESteamNetworkingSocketsDebugOutputType_Warning) {
                LOG_WARN("[net/gns] %s", msg);
            } else {
                LOG_INFO("[net/gns] %s", msg);
            }
        });

    g_instance = this;
    initialized_ = true;
    LOG_INFO("[net] Initialized GameNetworkingSockets");
    return true;
}

void NetworkManager::shutdown() {
    if (!initialized_) return;

    closeHost();

    // Close any remaining client connections
    for (auto& [handle, info] : connections_) {
        sockets_->CloseConnection(handle, 0, "shutdown", false);
    }
    connections_.clear();

    if (pollGroup_ != k_HSteamNetPollGroup_Invalid) {
        sockets_->DestroyPollGroup(pollGroup_);
        pollGroup_ = k_HSteamNetPollGroup_Invalid;
    }

    if (g_instance == this) g_instance = nullptr;

    GameNetworkingSockets_Kill();
    sockets_ = nullptr;
    initialized_ = false;
    LOG_INFO("[net] Shut down GameNetworkingSockets");
}

bool NetworkManager::host(uint16_t port) {
    if (!initialized_) return false;
    if (listenSocket_ != k_HSteamListenSocket_Invalid) {
        LOG_WARN("[net] Already hosting");
        return false;
    }

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&NetworkManager::connectionStatusCallback));

    listenSocket_ = sockets_->CreateListenSocketIP(addr, 1, &opt);
    if (listenSocket_ == k_HSteamListenSocket_Invalid) {
        LOG_ERROR("[net] Failed to create listen socket on port %d", port);
        return false;
    }

    pollGroup_ = sockets_->CreatePollGroup();
    if (pollGroup_ == k_HSteamNetPollGroup_Invalid) {
        LOG_ERROR("[net] Failed to create poll group");
        sockets_->CloseListenSocket(listenSocket_);
        listenSocket_ = k_HSteamListenSocket_Invalid;
        return false;
    }

    LOG_INFO("[net] Hosting on port %d", port);
    return true;
}

bool NetworkManager::connect(const std::string& address) {
    if (!initialized_) return false;

    SteamNetworkingIPAddr addr;
    if (!addr.ParseString(address.c_str())) {
        LOG_ERROR("[net] Invalid address: %s", address.c_str());
        return false;
    }

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&NetworkManager::connectionStatusCallback));

    // Create poll group for client if we don't have one
    if (pollGroup_ == k_HSteamNetPollGroup_Invalid) {
        pollGroup_ = sockets_->CreatePollGroup();
    }

    HSteamNetConnection conn = sockets_->ConnectByIPAddress(addr, 1, &opt);
    if (conn == k_HSteamNetConnection_Invalid) {
        LOG_ERROR("[net] Failed to initiate connection to %s", address.c_str());
        return false;
    }

    ConnectionInfo ci;
    ci.handle = conn;
    ci.remoteAddress = address;
    ci.state = k_ESteamNetworkingConnectionState_Connecting;
    connections_[conn] = ci;

    sockets_->SetConnectionPollGroup(conn, pollGroup_);

    LOG_INFO("[net] Connecting to %s", address.c_str());
    return true;
}

bool NetworkManager::send(HSteamNetConnection conn, const void* data, uint32_t size, bool reliable) {
    if (!initialized_ || !sockets_) return false;

    int flags = reliable
        ? k_nSteamNetworkingSend_Reliable
        : k_nSteamNetworkingSend_Unreliable;

    EResult result = sockets_->SendMessageToConnection(conn, data, size, flags, nullptr);
    return result == k_EResultOK;
}

void NetworkManager::broadcast(const void* data, uint32_t size, bool reliable) {
    for (auto& [handle, info] : connections_) {
        if (info.state == k_ESteamNetworkingConnectionState_Connected) {
            send(handle, data, size, reliable);
        }
    }
}

void NetworkManager::disconnect(HSteamNetConnection conn, int reason, const char* debug) {
    if (!initialized_ || !sockets_) return;
    sockets_->CloseConnection(conn, reason, debug, false);
    connections_.erase(conn);
}

void NetworkManager::closeHost() {
    if (listenSocket_ == k_HSteamListenSocket_Invalid) return;

    // Disconnect all clients
    for (auto& [handle, info] : connections_) {
        sockets_->CloseConnection(handle, 0, "server closing", false);
    }
    connections_.clear();

    sockets_->CloseListenSocket(listenSocket_);
    listenSocket_ = k_HSteamListenSocket_Invalid;

    if (pollGroup_ != k_HSteamNetPollGroup_Invalid) {
        sockets_->DestroyPollGroup(pollGroup_);
        pollGroup_ = k_HSteamNetPollGroup_Invalid;
    }

    LOG_INFO("[net] Stopped hosting");
}

void NetworkManager::poll() {
    if (!initialized_ || !sockets_) return;

    // Process connection state changes
    sockets_->RunCallbacks();

    // Drain incoming messages from the poll group
    if (pollGroup_ == k_HSteamNetPollGroup_Invalid) return;

    SteamNetworkingMessage_t* messages[64];
    int count = sockets_->ReceiveMessagesOnPollGroup(pollGroup_, messages, 64);
    for (int i = 0; i < count; ++i) {
        auto* msg = messages[i];

        if (onMessage) {
            NetworkMessage nm;
            nm.connection = msg->m_conn;
            nm.data.assign(
                static_cast<const uint8_t*>(msg->m_pData),
                static_cast<const uint8_t*>(msg->m_pData) + msg->m_cbSize);
            nm.channel = msg->m_nChannel;
            onMessage(std::move(nm));
        }

        msg->Release();
    }
}

bool NetworkManager::getConnectionInfo(HSteamNetConnection conn, ConnectionInfo& out) const {
    auto it = connections_.find(conn);
    if (it == connections_.end()) return false;
    out = it->second;
    return true;
}

bool NetworkManager::getConnectionStats(HSteamNetConnection conn, ConnectionStats& out) const {
    if (!sockets_) return false;

    SteamNetConnectionRealTimeStatus_t status;
    if (sockets_->GetConnectionRealTimeStatus(conn, &status, 0, nullptr) != k_EResultOK)
        return false;

    out.ping = static_cast<float>(status.m_nPing);
    out.packetLoss = 1.0f - status.m_flConnectionQualityLocal; // quality is 0..1 delivery rate
    out.bytesPerSecSent = status.m_flOutBytesPerSec;
    out.bytesPerSecRecv = status.m_flInBytesPerSec;
    return true;
}

std::vector<HSteamNetConnection> NetworkManager::connections() const {
    std::vector<HSteamNetConnection> result;
    result.reserve(connections_.size());
    for (auto& [handle, info] : connections_) {
        result.push_back(handle);
    }
    return result;
}

// --- Connection status callback ---

void NetworkManager::connectionStatusCallback(SteamNetConnectionStatusChangedCallback_t* info) {
    if (g_instance) {
        g_instance->onConnectionStatusChanged(info);
    }
}

void NetworkManager::onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    HSteamNetConnection conn = info->m_hConn;

    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            // Incoming connection on our listen socket — accept it
            if (listenSocket_ != k_HSteamListenSocket_Invalid) {
                if (sockets_->AcceptConnection(conn) != k_EResultOK) {
                    sockets_->CloseConnection(conn, 0, "accept failed", false);
                    break;
                }
                sockets_->SetConnectionPollGroup(conn, pollGroup_);

                ConnectionInfo ci;
                ci.handle = conn;
                ci.state = k_ESteamNetworkingConnectionState_Connecting;
                // Get remote address
                char addrBuf[SteamNetworkingIPAddr::k_cchMaxString];
                info->m_info.m_addrRemote.ToString(addrBuf, sizeof(addrBuf), true);
                ci.remoteAddress = addrBuf;
                connections_[conn] = ci;
            }
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected: {
            auto it = connections_.find(conn);
            if (it != connections_.end()) {
                it->second.state = k_ESteamNetworkingConnectionState_Connected;
            } else {
                ConnectionInfo ci;
                ci.handle = conn;
                ci.state = k_ESteamNetworkingConnectionState_Connected;
                char addrBuf[SteamNetworkingIPAddr::k_cchMaxString];
                info->m_info.m_addrRemote.ToString(addrBuf, sizeof(addrBuf), true);
                ci.remoteAddress = addrBuf;
                connections_[conn] = ci;
            }

            char addrBuf[SteamNetworkingIPAddr::k_cchMaxString];
            info->m_info.m_addrRemote.ToString(addrBuf, sizeof(addrBuf), true);
            LOG_INFO("[net] Connected: %s", addrBuf);

            if (onConnect) onConnect(conn);
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            int reason = info->m_info.m_eEndReason;
            LOG_INFO("[net] Disconnected: reason=%d (%s)", reason, info->m_info.m_szEndDebug);

            if (onDisconnect) onDisconnect(conn, reason);

            sockets_->CloseConnection(conn, 0, nullptr, false);
            connections_.erase(conn);
            break;
        }

        default:
            break;
    }
}

} // namespace bro::net
