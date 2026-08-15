#include "HttpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

// Reads exactly `count` more bytes into `buffer`, blocking as needed.
// Returns false on EOF/error before `count` bytes were read.
bool ReadExact(int fd, std::string& buffer, size_t count) {
    char chunk[4096];
    while (buffer.size() < count) {
        const size_t want = std::min(sizeof(chunk), count - buffer.size());
        const ssize_t got = ::recv(fd, chunk, want, 0);
        if (got <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(got));
    }
    return true;
}

// Reads until the buffer contains the header terminator, growing `buffer`
// as needed. Returns false on EOF/error/oversized headers before that.
bool ReadHeaders(int fd, std::string& buffer) {
    constexpr size_t kMaxHeaderBytes = 64 * 1024;
    char chunk[4096];
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) return false;
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(got));
    }
    return true;
}

void SendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote =
            ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (wrote <= 0) return;
        sent += static_cast<size_t>(wrote);
    }
}

const char* ReasonPhrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 202:
            return "Accepted";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        default:
            return "Internal Server Error";
    }
}

}  // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(int port, Handler handler, std::string& error) {
    if (running_.load()) {
        error = "server is already running";
        return false;
    }
    handler_ = std::move(handler);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket() failed";
        return false;
    }
    const int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only, deliberately: this server is never reachable from
    // outside the host (ROADMAP.md F1.1: "no service you don't control").
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        error = "bind(127.0.0.1:" + std::to_string(port) + ") failed";
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) != 0) {
        error = "listen() failed";
        ::close(fd);
        return false;
    }

    sockaddr_in bound{};
    socklen_t boundLength = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLength) ==
        0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    listen_fd_ = fd;
    running_.store(true);
    thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) return;
    const int fd = listen_fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (thread_.joinable()) thread_.join();
}

void HttpServer::AcceptLoop() {
    while (running_.load()) {
        const int fd = listen_fd_.load();
        if (fd < 0) break;
        sockaddr_in peer{};
        socklen_t peerLength = sizeof(peer);
        const int connection =
            ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (connection < 0) {
            // Stop() closing listen_fd_ unblocks accept() with an error;
            // treat any accept failure while stopped as a clean exit.
            if (!running_.load()) break;
            continue;
        }
        // Loopback-only defense in depth: refuse anything that somehow
        // reached us from a non-loopback source address.
        if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
            ::close(connection);
            continue;
        }
        const int noDelay = 1;
        ::setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &noDelay,
                     sizeof(noDelay));

        std::string buffer;
        if (!ReadHeaders(connection, buffer)) {
            ::close(connection);
            continue;
        }
        const size_t headerEnd = buffer.find("\r\n\r\n");
        const std::string headerBlock = buffer.substr(0, headerEnd);
        std::string body = buffer.substr(headerEnd + 4);

        std::istringstream lines(headerBlock);
        std::string requestLine;
        std::getline(lines, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r')
            requestLine.pop_back();
        std::istringstream requestLineStream(requestLine);
        std::string method;
        std::string path;
        std::string httpVersion;
        requestLineStream >> method >> path >> httpVersion;

        std::map<std::string, std::string> headers;
        std::string headerLine;
        while (std::getline(lines, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r')
                headerLine.pop_back();
            if (headerLine.empty()) continue;
            const size_t colon = headerLine.find(':');
            if (colon == std::string::npos) continue;
            headers[ToLower(Trim(headerLine.substr(0, colon)))] =
                Trim(headerLine.substr(colon + 1));
        }

        int statusCode = 200;
        std::string contentType = "application/json";
        std::string responseBody;

        if (method.empty() || path.empty()) {
            statusCode = 400;
            responseBody = "{\"error\":\"malformed request line\"}";
        } else {
            size_t contentLength = 0;
            const auto lengthHeader = headers.find("content-length");
            if (lengthHeader != headers.end()) {
                try {
                    contentLength =
                        static_cast<size_t>(std::stoull(lengthHeader->second));
                } catch (const std::exception&) {
                    contentLength = 0;
                }
            }
            constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;
            if (contentLength > kMaxBodyBytes) {
                statusCode = 413;
                responseBody = "{\"error\":\"request body too large\"}";
            } else if (!ReadExact(connection, body, contentLength)) {
                statusCode = 400;
                responseBody = "{\"error\":\"truncated request body\"}";
            } else {
                if (body.size() > contentLength) body.resize(contentLength);
                handler_(method, path, body, statusCode, contentType,
                         responseBody);
            }
        }

        std::ostringstream response;
        response << "HTTP/1.1 " << statusCode << " " << ReasonPhrase(statusCode)
                 << "\r\n"
                 << "Content-Type: " << contentType << "\r\n"
                 << "Content-Length: " << responseBody.size() << "\r\n"
                 << "Connection: close\r\n\r\n"
                 << responseBody;
        SendAll(connection, response.str());
        ::shutdown(connection, SHUT_WR);
        ::close(connection);
    }
}
