#pragma once

// A minimal, blocking HTTP/1.1 server for exactly one purpose: carrying
// JSON-RPC 2.0 POST requests for the MCP server (McpServer.h). It is
// deliberately not a general-purpose web server:
//   - Binds to 127.0.0.1 only, never 0.0.0.0 or a public interface. This is
//     a purely local server; nothing here ever accepts a non-loopback
//     connection (ROADMAP.md F1.1 hard constraint: "no service you don't
//     control").
//   - One request per connection (`Connection: close`); no keep-alive, no
//     chunked transfer-encoding, no TLS. A local agent process making one
//     JSON-RPC call at a time does not need any of that.
//   - Plain POSIX sockets only (sys/socket.h, not AppKit/Metal/FFmpeg), so
//     it builds and is testable on a plain Linux host as well as macOS.
//
// !! DO NOT EXPOSE THIS BEYOND LOOPBACK AS IT STANDS. !!
//
// Loopback-only binding is the *only* thing protecting the tool surface
// today. Anything that widens reach -- binding a non-loopback address,
// forwarding the port, tunnelling it, putting a proxy in front -- has to
// land together with the following, none of which exists yet:
//
//   - Authentication. There is none. Every request that reaches the handler
//     is fully trusted and can mutate the user's project.
//   - Origin/Content-Type validation. There is none, so a web page the user
//     merely visits can already drive real edits: a cross-origin POST with
//     Content-Type: text/plain is a CORS "simple request", so it is sent
//     without a preflight the server could refuse. The attacker cannot read
//     the reply, but the mutation still happens. Worth fixing even while
//     loopback-only.
//   - Request limits. The accept loop is strictly serial with no socket
//     timeouts, so one connection that opens and never sends wedges the
//     server for everyone and makes Stop() block forever on its join().

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class HttpServer {
public:
    // method/path/body in; statusCode/contentType/responseBody out. Called
    // synchronously on the server's accept thread -- one request is fully
    // handled before the next connection is accepted.
    using Handler = std::function<void(
        const std::string& method, const std::string& path,
        const std::string& body, int& statusCode, std::string& contentType,
        std::string& responseBody)>;

    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds 127.0.0.1:port (port 0 asks the OS for an ephemeral port --
    // see Port() to read back what was actually bound) and starts a
    // background accept loop. Returns false with `error` set on failure.
    bool Start(int port, Handler handler, std::string& error);

    // Stops the accept loop and joins its thread. Safe to call even if
    // Start() was never called or already failed.
    void Stop();

    bool IsRunning() const { return running_.load(); }

    // Valid only while IsRunning(); the actual bound port (useful when
    // Start() was called with port 0).
    int Port() const { return port_; }

private:
    void AcceptLoop();

    Handler handler_;
    // Read by the accept-loop thread on every iteration and written by
    // Stop() from whichever thread calls it; must be atomic.
    std::atomic<int> listen_fd_{-1};
    int port_ = 0;
    std::thread thread_;
    std::atomic_bool running_{false};
};
