#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

namespace bro::net {

/// Received message from a remote peer.
struct NetworkMessage {
    HSteamNetConnection connection;
    std::vector<uint8_t> data;
    int channel = 0;
};

/// Connection statistics snapshot.
struct ConnectionStats {
    float ping = 0.0f;           // round-trip time in ms
    float packetLoss = 0.0f;     // 0..1
    float bytesPerSecSent = 0.0f;
    float bytesPerSecRecv = 0.0f;
};

/// Tracks per-connection state visible to the engine.
struct ConnectionInfo {
    HSteamNetConnection handle = k_HSteamNetConnection_Invalid;
    std::string remoteAddress;
    int64_t userData = 0;
    ESteamNetworkingConnectionState state = k_ESteamNetworkingConnectionState_None;
};

/// Manages game networking using Valve's GameNetworkingSockets.
///
/// Supports two modes:
///  - Host: creates a listen socket, accepts connections from clients.
///  - Client: connects to a remote host by IP:port.
///
/// Integrates into the engine main loop via poll(), which drains incoming
/// messages and fires callbacks.
class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // Non-copyable
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    /// Initialize the GNS library. Call once at engine startup.
    bool init();

    /// Shut down the GNS library. Call at engine shutdown.
    void shutdown();

    /// Start listening for connections on the given port (host mode).
    /// Returns true on success.
    bool host(uint16_t port);

    /// Connect to a remote host (client mode).
    /// address is "ip:port" (e.g. "127.0.0.1:27015").
    /// Returns true if the connection attempt was initiated.
    bool connect(const std::string& address);

    /// Send data to a specific connection.
    /// reliable: if true, uses reliable ordered delivery; otherwise unreliable.
    bool send(HSteamNetConnection conn, const void* data, uint32_t size, bool reliable);

    /// Send data to all connected peers (broadcast).
    void broadcast(const void* data, uint32_t size, bool reliable);

    /// Disconnect a specific connection.
    void disconnect(HSteamNetConnection conn, int reason = 0, const char* debug = "");

    /// Close the listen socket (stop hosting). Disconnects all clients.
    void closeHost();

    /// Poll for incoming messages and connection state changes.
    /// Call once per frame from the main loop.
    void poll();

    /// Get info about a connection.
    bool getConnectionInfo(HSteamNetConnection conn, ConnectionInfo& out) const;

    /// Get real-time connection statistics.
    bool getConnectionStats(HSteamNetConnection conn, ConnectionStats& out) const;

    /// Get all active connection handles.
    std::vector<HSteamNetConnection> connections() const;

    /// True if we have a listen socket open.
    bool isHosting() const { return listenSocket_ != k_HSteamListenSocket_Invalid; }

    /// True if the library is initialized.
    bool isInitialized() const { return initialized_; }

    // --- Callbacks (set by JS bindings) ---
    std::function<void(HSteamNetConnection)> onConnect;
    std::function<void(HSteamNetConnection, int reason)> onDisconnect;
    std::function<void(NetworkMessage&&)> onMessage;

private:
    static void connectionStatusCallback(SteamNetConnectionStatusChangedCallback_t* info);
    void onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;
    bool initialized_ = false;

    // Track active connections
    std::unordered_map<HSteamNetConnection, ConnectionInfo> connections_;
};

} // namespace bro::net
