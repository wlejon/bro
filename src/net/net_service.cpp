#include "net/net_service.h"
#include "util/log.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <chrono>
#include <cstring>

namespace bro::net {

NetService* NetService::s_instance = nullptr;

// =============================================================================
// NetSubscriber
// =============================================================================

NetSubscriber::NetSubscriber(NetService* service, uint32_t id)
    : service_(service), id_(id) {}

static NetCommand* makeCmd(uint32_t sid, NetCommand::Type t) {
    auto* c = new NetCommand();
    c->type = t;
    c->subscriberId = sid;
    return c;
}

void NetSubscriber::host(uint16_t port) {
    auto* c = makeCmd(id_, NetCommand::Host);
    c->port = port;
    service_->postCommand(c);
}

void NetSubscriber::connect(const std::string& address) {
    auto* c = makeCmd(id_, NetCommand::Connect);
    c->address = address;
    service_->postCommand(c);
}

bool NetSubscriber::send(uint32_t conn, const void* data, uint32_t size, bool reliable) {
    auto* c = makeCmd(id_, NetCommand::Send);
    c->connection = conn;
    c->reliable = reliable;
    c->data.assign(static_cast<const uint8_t*>(data),
                   static_cast<const uint8_t*>(data) + size);
    service_->postCommand(c);
    return true;
}

void NetSubscriber::broadcast(const void* data, uint32_t size, bool reliable) {
    auto* c = makeCmd(id_, NetCommand::Broadcast);
    c->reliable = reliable;
    c->data.assign(static_cast<const uint8_t*>(data),
                   static_cast<const uint8_t*>(data) + size);
    service_->postCommand(c);
}

void NetSubscriber::disconnect(uint32_t conn, int reason) {
    auto* c = makeCmd(id_, NetCommand::Disconnect);
    c->connection = conn;
    c->reason = reason;
    service_->postCommand(c);
}

void NetSubscriber::closeHost() {
    service_->postCommand(makeCmd(id_, NetCommand::CloseHost));
}

void NetSubscriber::poll() {
    while (NetEvent* ev = events_.pop()) {
        switch (ev->type) {
            case NetEvent::HostResult:
                if (onHostResult) onHostResult(ev->success);
                break;
            case NetEvent::ConnectResult:
                if (onConnectResult) onConnectResult(ev->success);
                break;
            case NetEvent::Connected:
                if (onConnect) onConnect(ev->connection);
                break;
            case NetEvent::Disconnected:
                if (onDisconnect) onDisconnect(ev->connection, ev->reason);
                break;
            case NetEvent::Message:
                if (onMessage) {
                    NetworkMessage m;
                    m.connection = ev->connection;
                    m.data = std::move(ev->data);
                    onMessage(std::move(m));
                }
                break;
        }
        delete ev;
    }
}

bool NetSubscriber::getConnectionStats(uint32_t conn, ConnectionStats& out) const {
    return service_->getConnectionStats(conn, out);
}

// =============================================================================
// NetService
// =============================================================================

NetService::NetService() {
    if (s_instance) {
        LOG_ERROR("[net] NetService: second instance created — there can be only one.");
    }
    s_instance = this;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&NetService::threadMain, this);
}

NetService::~NetService() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();

    // Any stragglers pushed after the thread drained and exited: just free
    // the command nodes. The thread's shutdown already handled subscriber
    // ownership, so we must NOT delete subscriberPtr here (double-free risk).
    NetCommand* head = cmdHead_.exchange(nullptr, std::memory_order_acquire);
    while (head) {
        NetCommand* next = head->next;
        delete head;
        head = next;
    }

    if (s_instance == this) s_instance = nullptr;
}

NetSubscriber* NetService::createSubscriber() {
    uint32_t id = nextSubscriberId_.fetch_add(1, std::memory_order_relaxed);
    auto* sub = new NetSubscriber(this, id);
    auto* c = new NetCommand();
    c->type = NetCommand::Register;
    c->subscriberId = id;
    c->subscriberPtr = sub;
    postCommand(c);
    return sub;
}

void NetService::destroySubscriber(NetSubscriber* sub) {
    if (!sub) return;
    auto* c = new NetCommand();
    c->type = NetCommand::Unregister;
    c->subscriberId = sub->id();
    c->subscriberPtr = sub;
    postCommand(c);
}

bool NetService::getConnectionStats(uint32_t conn, ConnectionStats& out) const {
    if (!sockets_) return false;
    SteamNetConnectionRealTimeStatus_t status;
    if (sockets_->GetConnectionRealTimeStatus(conn, &status, 0, nullptr) != k_EResultOK)
        return false;
    out.ping = static_cast<float>(status.m_nPing);
    out.packetLoss = 1.0f - status.m_flConnectionQualityLocal;
    out.bytesPerSecSent = status.m_flOutBytesPerSec;
    out.bytesPerSecRecv = status.m_flInBytesPerSec;
    return true;
}

// ---------------------------------------------------------------------------
// Lock-free MPSC push — CAS the node onto the stack head.
// ---------------------------------------------------------------------------
void NetService::postCommand(NetCommand* cmd) {
    NetCommand* old = cmdHead_.load(std::memory_order_relaxed);
    do {
        cmd->next = old;
    } while (!cmdHead_.compare_exchange_weak(
        old, cmd,
        std::memory_order_release,
        std::memory_order_relaxed));
}

void NetService::postEventTo(uint32_t subscriberId, NetEvent* ev) {
    // Service-thread only. subscribers_ is owned by this thread.
    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) { delete ev; return; }
    if (!it->second->events_.push(ev)) {
        LOG_WARN("[net] Event queue full for subscriber %u — dropping event", subscriberId);
        delete ev;
    }
}

// ---------------------------------------------------------------------------
// GNS C callback — single entry point routed via s_instance. Fires on the
// thread that called RunCallbacks(), which is always the service thread.
// ---------------------------------------------------------------------------
void NetService::statusCallback(SteamNetConnectionStatusChangedCallback_t* info) {
    if (s_instance) s_instance->onStatus(info);
}

void NetService::onStatus(SteamNetConnectionStatusChangedCallback_t* info) {
    HSteamNetConnection conn = info->m_hConn;

    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            auto lit = listenOwner_.find(info->m_info.m_hListenSocket);
            if (lit != listenOwner_.end()) {
                uint32_t owner = lit->second;
                if (sockets_->AcceptConnection(conn) != k_EResultOK) {
                    sockets_->CloseConnection(conn, 0, "accept failed", false);
                    break;
                }
                auto pgIt = subscriberPollGroup_.find(owner);
                if (pgIt != subscriberPollGroup_.end()) {
                    sockets_->SetConnectionPollGroup(conn, pgIt->second);
                }
                connectionOwner_[conn] = owner;
            }
            break;
        }
        case k_ESteamNetworkingConnectionState_Connected: {
            auto it = connectionOwner_.find(conn);
            if (it == connectionOwner_.end()) break;
            auto* ev = new NetEvent{};
            ev->type = NetEvent::Connected;
            ev->connection = static_cast<uint32_t>(conn);
            postEventTo(it->second, ev);
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            int reason = info->m_info.m_eEndReason;
            auto it = connectionOwner_.find(conn);
            if (it != connectionOwner_.end()) {
                auto* ev = new NetEvent{};
                ev->type = NetEvent::Disconnected;
                ev->connection = static_cast<uint32_t>(conn);
                ev->reason = reason;
                postEventTo(it->second, ev);
                connectionOwner_.erase(it);
            }
            sockets_->CloseConnection(conn, 0, nullptr, false);
            break;
        }
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Subscriber teardown helper (service thread).
// ---------------------------------------------------------------------------
static void closeSubscriberResources(ISteamNetworkingSockets* sockets,
                                     uint32_t sid,
                                     std::unordered_map<uint32_t, uint32_t>& connectionOwner,
                                     std::unordered_map<HSteamListenSocket, uint32_t>& listenOwner,
                                     std::unordered_map<uint32_t, HSteamListenSocket>& subscriberListen,
                                     std::unordered_map<uint32_t, HSteamNetPollGroup>& subscriberPollGroup)
{
    for (auto it = connectionOwner.begin(); it != connectionOwner.end(); ) {
        if (it->second == sid) {
            sockets->CloseConnection(it->first, 0, "subscriber gone", false);
            it = connectionOwner.erase(it);
        } else ++it;
    }
    auto ls = subscriberListen.find(sid);
    if (ls != subscriberListen.end()) {
        sockets->CloseListenSocket(ls->second);
        listenOwner.erase(ls->second);
        subscriberListen.erase(ls);
    }
    auto pg = subscriberPollGroup.find(sid);
    if (pg != subscriberPollGroup.end()) {
        sockets->DestroyPollGroup(pg->second);
        subscriberPollGroup.erase(pg);
    }
}

// ---------------------------------------------------------------------------
// Command handling (service thread)
// ---------------------------------------------------------------------------
void NetService::handleCommand(NetCommand& cmd) {
    switch (cmd.type) {
        case NetCommand::Register: {
            subscribers_[cmd.subscriberId] = cmd.subscriberPtr;
            break;
        }
        case NetCommand::Unregister: {
            closeSubscriberResources(sockets_, cmd.subscriberId,
                                     connectionOwner_, listenOwner_,
                                     subscriberListen_, subscriberPollGroup_);
            subscribers_.erase(cmd.subscriberId);
            delete cmd.subscriberPtr;
            break;
        }
        case NetCommand::Host: {
            if (subscriberListen_.count(cmd.subscriberId)) {
                auto* ev = new NetEvent{};
                ev->type = NetEvent::HostResult;
                ev->success = false;
                postEventTo(cmd.subscriberId, ev);
                break;
            }
            SteamNetworkingIPAddr addr;
            addr.Clear();
            addr.m_port = cmd.port;

            SteamNetworkingConfigValue_t opt;
            opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                       reinterpret_cast<void*>(&NetService::statusCallback));

            HSteamListenSocket sock = sockets_->CreateListenSocketIP(addr, 1, &opt);
            bool ok = (sock != k_HSteamListenSocket_Invalid);
            if (ok) {
                HSteamNetPollGroup pg = sockets_->CreatePollGroup();
                subscriberListen_[cmd.subscriberId] = sock;
                subscriberPollGroup_[cmd.subscriberId] = pg;
                listenOwner_[sock] = cmd.subscriberId;
                LOG_INFO("[net] Subscriber %u hosting on port %u",
                         cmd.subscriberId, cmd.port);
            } else {
                LOG_ERROR("[net] Subscriber %u host failed on port %u",
                          cmd.subscriberId, cmd.port);
            }
            auto* ev = new NetEvent{};
            ev->type = NetEvent::HostResult;
            ev->success = ok;
            postEventTo(cmd.subscriberId, ev);
            break;
        }
        case NetCommand::Connect: {
            SteamNetworkingIPAddr addr;
            if (!addr.ParseString(cmd.address.c_str())) {
                auto* ev = new NetEvent{};
                ev->type = NetEvent::ConnectResult;
                ev->success = false;
                postEventTo(cmd.subscriberId, ev);
                break;
            }
            SteamNetworkingConfigValue_t opt;
            opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                       reinterpret_cast<void*>(&NetService::statusCallback));

            HSteamNetConnection conn = sockets_->ConnectByIPAddress(addr, 1, &opt);
            bool ok = (conn != k_HSteamNetConnection_Invalid);
            if (ok) {
                auto pgIt = subscriberPollGroup_.find(cmd.subscriberId);
                HSteamNetPollGroup pg = (pgIt == subscriberPollGroup_.end())
                    ? sockets_->CreatePollGroup()
                    : pgIt->second;
                if (pgIt == subscriberPollGroup_.end())
                    subscriberPollGroup_[cmd.subscriberId] = pg;
                sockets_->SetConnectionPollGroup(conn, pg);
                connectionOwner_[conn] = cmd.subscriberId;
                LOG_INFO("[net] Subscriber %u connecting to %s",
                         cmd.subscriberId, cmd.address.c_str());
            }
            auto* ev = new NetEvent{};
            ev->type = NetEvent::ConnectResult;
            ev->success = ok;
            postEventTo(cmd.subscriberId, ev);
            break;
        }
        case NetCommand::Send: {
            int flags = cmd.reliable
                ? k_nSteamNetworkingSend_Reliable
                : k_nSteamNetworkingSend_Unreliable;
            sockets_->SendMessageToConnection(cmd.connection,
                                              cmd.data.data(),
                                              static_cast<uint32_t>(cmd.data.size()),
                                              flags, nullptr);
            break;
        }
        case NetCommand::Broadcast: {
            int flags = cmd.reliable
                ? k_nSteamNetworkingSend_Reliable
                : k_nSteamNetworkingSend_Unreliable;
            for (auto& [conn, owner] : connectionOwner_) {
                if (owner == cmd.subscriberId) {
                    sockets_->SendMessageToConnection(conn,
                                                      cmd.data.data(),
                                                      static_cast<uint32_t>(cmd.data.size()),
                                                      flags, nullptr);
                }
            }
            break;
        }
        case NetCommand::Disconnect: {
            sockets_->CloseConnection(cmd.connection, cmd.reason, "", false);
            connectionOwner_.erase(cmd.connection);
            break;
        }
        case NetCommand::CloseHost: {
            auto it = subscriberListen_.find(cmd.subscriberId);
            if (it == subscriberListen_.end()) break;
            for (auto cit = connectionOwner_.begin(); cit != connectionOwner_.end(); ) {
                if (cit->second == cmd.subscriberId) {
                    sockets_->CloseConnection(cit->first, 0, "host closing", false);
                    cit = connectionOwner_.erase(cit);
                } else ++cit;
            }
            sockets_->CloseListenSocket(it->second);
            listenOwner_.erase(it->second);
            subscriberListen_.erase(it);
            auto pgIt = subscriberPollGroup_.find(cmd.subscriberId);
            if (pgIt != subscriberPollGroup_.end()) {
                sockets_->DestroyPollGroup(pgIt->second);
                subscriberPollGroup_.erase(pgIt);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Drain the MPSC stack and reverse LIFO → FIFO for ordered processing.
// ---------------------------------------------------------------------------
static NetCommand* drainAndReverse(std::atomic<NetCommand*>& head) {
    NetCommand* lifo = head.exchange(nullptr, std::memory_order_acquire);
    NetCommand* fifo = nullptr;
    while (lifo) {
        NetCommand* next = lifo->next;
        lifo->next = fifo;
        fifo = lifo;
        lifo = next;
    }
    return fifo;
}

// ---------------------------------------------------------------------------
// Service thread main
// ---------------------------------------------------------------------------
void NetService::threadMain() {
    SteamNetworkingErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        LOG_ERROR("[net] GameNetworkingSockets_Init failed: %s", errMsg);
        return;
    }
    sockets_ = SteamNetworkingSockets();
    if (!sockets_) {
        LOG_ERROR("[net] Failed to get ISteamNetworkingSockets interface");
        GameNetworkingSockets_Kill();
        return;
    }
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

    LOG_INFO("[net] NetService started");

    while (running_.load(std::memory_order_acquire)) {
        // 1. Drain and process commands (FIFO).
        NetCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            NetCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }

        // 2. Run GNS callbacks (fires onStatus for state changes).
        sockets_->RunCallbacks();

        // 3. Drain incoming messages for each subscriber's poll group.
        for (auto& [sid, pg] : subscriberPollGroup_) {
            SteamNetworkingMessage_t* msgs[64];
            int n = sockets_->ReceiveMessagesOnPollGroup(pg, msgs, 64);
            for (int i = 0; i < n; ++i) {
                auto* m = msgs[i];
                auto* ev = new NetEvent{};
                ev->type = NetEvent::Message;
                ev->connection = static_cast<uint32_t>(m->m_conn);
                ev->data.assign(static_cast<const uint8_t*>(m->m_pData),
                                static_cast<const uint8_t*>(m->m_pData) + m->m_cbSize);
                postEventTo(sid, ev);
                m->Release();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Drain any commands that arrived between our last loop iteration and
    // the running_=false observation — so late Unregister commands get
    // processed instead of leaking subscribers (or causing double-free
    // during destructor cleanup).
    {
        NetCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            NetCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }
    }

    // Shutdown: close all resources and free remaining subscribers.
    for (auto& [sid, sock] : subscriberListen_) sockets_->CloseListenSocket(sock);
    for (auto& [conn, owner] : connectionOwner_) sockets_->CloseConnection(conn, 0, "shutdown", false);
    for (auto& [sid, pg] : subscriberPollGroup_) sockets_->DestroyPollGroup(pg);
    for (auto& [sid, sub] : subscribers_) delete sub;
    subscriberListen_.clear();
    listenOwner_.clear();
    connectionOwner_.clear();
    subscriberPollGroup_.clear();
    subscribers_.clear();

    sockets_ = nullptr;
    GameNetworkingSockets_Kill();
    LOG_INFO("[net] NetService stopped");
}

} // namespace bro::net
