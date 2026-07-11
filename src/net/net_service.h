#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

namespace bro::net {

/// Received message from a remote peer.
struct NetworkMessage {
    uint32_t connection;
    std::vector<uint8_t> data;
    int channel = 0;
};

/// Connection statistics snapshot.
struct ConnectionStats {
    float ping = 0.0f;
    float packetLoss = 0.0f;
    float bytesPerSecSent = 0.0f;
    float bytesPerSecRecv = 0.0f;
};

/// Per-connection info visible to subscribers.
struct ConnectionInfo {
    uint32_t handle = 0;
    std::string remoteAddress;
    int64_t userData = 0;
    int state = 0; // ESteamNetworkingConnectionState
};

class NetService;

// ---------------------------------------------------------------------------
// Command (subscriber thread → service thread)
// ---------------------------------------------------------------------------
struct NetCommand {
    enum Type : uint8_t {
        Register,      // new subscriber joined (ptr set)
        Unregister,    // subscriber leaving (ptr set)
        Host,          // port
        Connect,       // address
        Send,          // conn, data, reliable
        Broadcast,     // data, reliable
        Disconnect,    // conn, reason
        CloseHost,
    };
    Type type;
    uint32_t subscriberId = 0;
    uint16_t port = 0;
    uint32_t connection = 0;
    int reason = 0;
    bool reliable = false;
    std::string address;
    std::vector<uint8_t> data;
    class NetSubscriber* subscriberPtr = nullptr; // Register/Unregister
};

// ---------------------------------------------------------------------------
// Event (service thread → subscriber thread)
// ---------------------------------------------------------------------------
struct NetEvent {
    enum Type : uint8_t {
        HostResult,      // success
        ConnectResult,   // success (initiated)
        Connected,       // connection handle
        Disconnected,    // connection, reason
        Message,         // connection, data
    };
    Type type;
    bool success = false;
    uint32_t connection = 0;
    int reason = 0;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
// SPSC ring buffer (pointer slots; template to avoid coupling to Message type)
// ---------------------------------------------------------------------------
template <typename T, size_t Capacity = 1024>
class Spsc {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
public:
    bool push(T* item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) & (Capacity - 1);
        if (next == head_.load(std::memory_order_acquire)) return false;
        slots_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }
    T* pop() {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return nullptr;
        T* item = slots_[head];
        slots_[head] = nullptr;
        head_.store((head + 1) & (Capacity - 1), std::memory_order_release);
        return item;
    }
    void clear() { while (T* m = pop()) delete m; }
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T* slots_[Capacity]{};
};

// ---------------------------------------------------------------------------
// NetSubscriber — a per-thread handle into the NetService.
//
// Each JS context (main thread, each worker) creates one. Method calls push
// commands; poll() drains the event queue and fires callbacks synchronously
// on the calling thread.
// ---------------------------------------------------------------------------
class NetSubscriber {
public:
    uint32_t id() const { return id_; }

    // Commands (non-blocking; result comes back via the corresponding event)
    void host(uint16_t port);
    void connect(const std::string& address);
    bool send(uint32_t conn, const void* data, uint32_t size, bool reliable);
    void broadcast(const void* data, uint32_t size, bool reliable);
    void disconnect(uint32_t conn, int reason = 0);
    void closeHost();

    /// Drain event queue and fire callbacks. Call once per frame.
    void poll();

    /// Real-time connection stats. Uses the GNS interface directly — safe
    /// because ISteamNetworkingSockets is documented thread-safe for reads.
    bool getConnectionStats(uint32_t conn, ConnectionStats& out) const;

    // Callbacks — bindings set these. Fire on the subscriber's thread
    // during poll().
    std::function<void(bool success)> onHostResult;
    std::function<void(bool success)> onConnectResult;
    std::function<void(uint32_t conn)> onConnect;
    std::function<void(uint32_t conn, int reason)> onDisconnect;
    std::function<void(NetworkMessage&& msg)> onMessage;

private:
    friend class NetService;
    NetSubscriber(NetService* service, uint32_t id);

    NetService* service_;
    uint32_t id_;
    Spsc<NetEvent> events_;
};

// ---------------------------------------------------------------------------
// NetService — owns GNS on a dedicated thread.
//
// Construct once per process (Engine owns it). Subscribers are created and
// destroyed by JS bindings as contexts come and go. GNS init happens on the
// service thread at startup.
// ---------------------------------------------------------------------------
class NetService {
public:
    NetService();
    ~NetService();

    NetService(const NetService&) = delete;
    NetService& operator=(const NetService&) = delete;

    /// Allocate a subscriber and register it with the service. The returned
    /// pointer is non-owning — call destroySubscriber() to release. Safe to
    /// call from any thread.
    NetSubscriber* createSubscriber();

    /// Release a subscriber. After this call the pointer is invalid; the
    /// service thread will tear down any sockets/connections it owns and
    /// free the subscriber asynchronously. Safe to call from any thread, but
    /// do not call poll() on the subscriber after this.
    void destroySubscriber(NetSubscriber* sub);

    /// Real-time stats (thread-safe read through GNS).
    bool getConnectionStats(uint32_t conn, ConnectionStats& out) const;

private:
    friend class NetSubscriber;

    void threadMain();
    void postCommand(NetCommand* cmd);
    void postEventTo(uint32_t subscriberId, NetEvent* ev);

    // Called from the service thread only.
    void handleCommand(NetCommand& cmd);
    static void statusCallback(SteamNetConnectionStatusChangedCallback_t* info);
    void onStatus(SteamNetConnectionStatusChangedCallback_t* info);

    // --- Service-thread-only state (never touched from other threads) ---
    ISteamNetworkingSockets* sockets_ = nullptr;
    std::unordered_map<uint32_t, NetSubscriber*> subscribers_; // id → subscriber
    std::unordered_map<uint32_t, uint32_t> connectionOwner_;       // conn → subscriberId
    std::unordered_map<HSteamListenSocket, uint32_t> listenOwner_; // socket → subscriberId
    std::unordered_map<uint32_t, HSteamListenSocket> subscriberListen_;
    std::unordered_map<uint32_t, HSteamNetPollGroup> subscriberPollGroup_;

    // --- Command ingress + lifecycle (guarded by m_) ---
    //
    // Producers (any thread) lock, push, notify. The service thread swaps the
    // queue out under the lock and processes outside it. m_ also guards
    // sockets_ for OUTSIDE readers (getConnectionStats from any thread) —
    // including across shutdown, where GameNetworkingSockets_Kill() runs
    // under the lock so an in-flight stats call can never race it. The
    // service thread itself is the only writer and reads sockets_ freely.
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<NetCommand>> cmdQueue_;
    bool running_ = false;

    std::thread thread_;
    std::atomic<uint32_t> nextSubscriberId_{1};

    // One service per process; the GNS C callback routes through this.
    static NetService* s_instance;
};

} // namespace bro::net
