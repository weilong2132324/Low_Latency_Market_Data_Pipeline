/*
 * ws_server.hpp - minimal hand-rolled RFC 6455 WebSocket server.
 *
 * Responsibility: push JSON telemetry frames from telemetry_bridge to
 * any connected browser, with no third-party WebSocket library (same
 * no-external-deps rule the rest of this project follows). This is a
 * SERVER-PUSH-ONLY implementation: it performs the opening handshake,
 * then only ever sends unmasked text frames; it does not parse or act
 * on anything the client sends after the handshake (a browser's
 * WebSocket API never needs to send anything for this project's
 * one-way telemetry feed, so there is nothing to lose by not reading
 * client->server frames - an accepted scope limit, not an oversight).
 *
 * Handshake (RFC 6455 Section 4.2.2): the client sends an HTTP
 * upgrade request with a `Sec-WebSocket-Key` header; the server
 * replies `HTTP/1.1 101 Switching Protocols` with
 * `Sec-WebSocket-Accept: base64(sha1(key + "258EAFA5-E914-47DA-95CA-
 * C5AB0DC85B11"))` - that fixed GUID is mandated by the spec, not a
 * secret.
 *
 * Framing (RFC 6455 Section 5.2): server->client frames are sent
 * UNMASKED (only client->server frames are masked, by spec) with a
 * single FIN'd text frame (opcode 0x1) per message - no fragmentation,
 * since every telemetry payload here is small.
 *
 * Thread-safety: one accept thread; broadcast_text() is safe to call
 * from any thread (guards the client list with a mutex). Each client's
 * socket write happens under that same lock - telemetry fan-out is not
 * a hot path (this project sends at most tens of messages/second to
 * the browser), so a coarse lock is the right tool here, matching the
 * same "not every lock is a bug" judgment call documented throughout
 * this codebase (queue_mutex.hpp, snapshot_server.hpp).
 */

#ifndef PULSEFORGE_WS_SERVER_HPP
#define PULSEFORGE_WS_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Socket; /* common.hpp */

class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer &)            = delete;
    WebSocketServer &operator=(const WebSocketServer &) = delete;

    /* Binds, listens, and spawns the accept thread. Throws SystemError
     * on failure. */
    void start(int port);

    /* Stops the accept thread and closes every connected client. Safe
     * to call more than once. */
    void stop();

    /* Sends one text frame containing `payload` to every currently
     * connected client; silently drops any client whose write fails
     * (it is presumed disconnected and removed from the list). */
    void broadcast_text(const std::string &payload);

    size_t client_count() const;

private:
    void accept_loop(int port);
    bool perform_handshake(int client_fd);

    mutable std::mutex     clients_mu_;
    std::vector<int>       clients_;

    std::atomic<bool>       stop_{false};
    std::thread             thread_;
    std::unique_ptr<Socket> listen_sock_;
};

#endif /* PULSEFORGE_WS_SERVER_HPP */
