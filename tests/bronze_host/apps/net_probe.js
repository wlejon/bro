// The Networking & Remote Transport probe for bronze_host.
//
// Tests:
// - bro.net API methods, properties, and event handlers
// - WebSocket Web API constructor, constants, readyState, binaryType, methods, events
// - CloseEvent and MessageEvent global constructors
// - Remote HTTP fetch / XMLHttpRequest integration

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// ---------------------------------------------------------------------------
// 1. bro.net API Surface
// ---------------------------------------------------------------------------

say('bro.exists', typeof bro === 'object');
say('bro.net.exists', typeof bro.net === 'object');
say('bro.net.has_init', typeof bro.net.init === 'function');
say('bro.net.has_host', typeof bro.net.host === 'function');
say('bro.net.has_connect', typeof bro.net.connect === 'function');
say('bro.net.has_disconnect', typeof bro.net.disconnect === 'function');
say('bro.net.has_send', typeof bro.net.send === 'function');
say('bro.net.has_broadcast', typeof bro.net.broadcast === 'function');
say('bro.net.has_close', typeof bro.net.close === 'function');
say('bro.net.has_closeHost', typeof bro.net.closeHost === 'function');
say('bro.net.has_isHosting', typeof bro.net.isHosting === 'function');
say('bro.net.has_connections', typeof bro.net.connections === 'function');
say('bro.net.has_stats', typeof bro.net.stats === 'function');
say('bro.net.has_addEventListener', typeof bro.net.addEventListener === 'function');
say('bro.net.has_removeEventListener', typeof bro.net.removeEventListener === 'function');

// Initial state
say('bro.net.init_result', bro.net.init());
say('bro.net.isHosting_initial', bro.net.isHosting());
const conns = bro.net.connections();
say('bro.net.connections_isArray', Array.isArray(conns));
say('bro.net.connections_len', conns.length);
say('bro.net.stats_invalid', bro.net.stats(99999) === null);

// Host initiation
const hostOk = bro.net.host(27088);
say('bro.net.host_result', hostOk);

// Event handlers assignment
bro.net.onConnect = function(connId) {};
bro.net.onDisconnect = function(connId, reason) {};
bro.net.onMessage = function(connId, data, channel) {};
say('bro.net.onConnect_set', typeof bro.net.onConnect === 'function');
say('bro.net.onDisconnect_set', typeof bro.net.onDisconnect === 'function');
say('bro.net.onMessage_set', typeof bro.net.onMessage === 'function');

// ---------------------------------------------------------------------------
// 2. WebSocket Web API
// ---------------------------------------------------------------------------

say('ws.global_exists', typeof WebSocket === 'function');
say('ws.const_connecting', WebSocket.CONNECTING === 0);
say('ws.const_open', WebSocket.OPEN === 1);
say('ws.const_closing', WebSocket.CLOSING === 2);
say('ws.const_closed', WebSocket.CLOSED === 3);

say('closeEvent.exists', typeof CloseEvent === 'function');
say('messageEvent.exists', typeof MessageEvent === 'function');

// WebSocket instance creation
const ws = new WebSocket('ws://127.0.0.1:27088', 'test-proto');
say('ws.instance_exists', ws !== null && typeof ws === 'object');
say('ws.instance_connecting', ws.CONNECTING === 0);
say('ws.instance_open', ws.OPEN === 1);
say('ws.instance_closing', ws.CLOSING === 2);
say('ws.instance_closed', ws.CLOSED === 3);
say('ws.readyState_initial', ws.readyState === 0);
say('ws.url', ws.url);
say('ws.protocol', ws.protocol);
say('ws.binaryType_default', ws.binaryType);
say('ws.bufferedAmount', ws.bufferedAmount);

// Mutate binaryType
ws.binaryType = 'arraybuffer';
say('ws.binaryType_updated', ws.binaryType);

// Event handler assignment & methods
ws.onopen = function(e) {};
ws.onmessage = function(e) {};
ws.onerror = function(e) {};
ws.onclose = function(e) {};
say('ws.has_send', typeof ws.send === 'function');
say('ws.has_close', typeof ws.close === 'function');
say('ws.has_addEventListener', typeof ws.addEventListener === 'function');
say('ws.has_removeEventListener', typeof ws.removeEventListener === 'function');

// Close the WebSocket
ws.close(1000, 'Test Done');
say('ws.readyState_after_close', ws.readyState === 3);

// ---------------------------------------------------------------------------
// 3. Remote HTTP fetch() & XMLHttpRequest
// ---------------------------------------------------------------------------

say('fetch.exists', typeof fetch === 'function');
say('xhr.exists', typeof XMLHttpRequest === 'function');

// Test data URL fetch (instant inline resolution)
fetch('data:text/plain;base64,SGVsbG8gQnJvbnplIE5ldHdvcmtpbmch').then(function(res) {
    say('fetch.data_ok', res.ok);
    say('fetch.data_status', res.status);
    return res.text();
}).then(function(text) {
    say('fetch.data_body', text);
});

// Close host at end of setup
bro.net.close();
say('bro.net.isHosting_after_close', bro.net.isHosting());

say('net_probe.done', 1);
