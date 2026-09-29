#include "unix_socket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace wizard {

namespace {
constexpr size_t kReadBufSize = 4096;
}

UnixSocket::UnixSocket(int fd) : fd_(fd) {}

UnixSocket::~UnixSocket() {
    if (fd_ >= 0) close(fd_);
}

UnixSocket::UnixSocket(UnixSocket&& other) noexcept
    : fd_(other.fd_), parser_(std::move(other.parser_)) {
    other.fd_ = -1;
}

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_;
        parser_ = std::move(other.parser_);
        other.fd_ = -1;
    }
    return *this;
}

// Writes the full encoded frame, looping over partial writes.
bool UnixSocket::send(const Message& msg) {
    auto bytes = encode_frame(msg);
    size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = ::write(fd_, bytes.data() + sent, bytes.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Single read() call, feeds the parser, drains all complete messages.
std::optional<std::vector<Message>> UnixSocket::receive() {
    uint8_t buf[kReadBufSize];
    ssize_t n = ::read(fd_, buf, sizeof(buf));

    if (n == 0) return std::nullopt;  // peer closed
    if (n < 0) {
        if (errno == EINTR) return std::vector<Message>{};  // retry later, no data lost
        return std::nullopt;  // real error
    }

    parser_.feed(buf, static_cast<size_t>(n));

    std::vector<Message> out;
    while (auto msg = parser_.try_parse()) {
        out.push_back(std::move(*msg));
    }
    return out;
}

// Creates, binds, listens, and blocks for a single incoming connection.
std::optional<UnixSocket> listen_and_accept(const std::string& path) {
    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return std::nullopt;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ::unlink(path.c_str());  // remove stale socket file if present

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd);
        return std::nullopt;
    }
    if (::listen(listen_fd, 1) < 0) {
        close(listen_fd);
        return std::nullopt;
    }

    int client_fd = ::accept(listen_fd, nullptr, nullptr);
    close(listen_fd);  // V1: single client, listening socket no longer needed
    if (client_fd < 0) return std::nullopt;

    return UnixSocket(client_fd);
}

// Creates a socket and connects to an already-listening server.
UnixSocket connect_to(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("failed to create socket");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("failed to connect to " + path);
    }
    return UnixSocket(fd);
}

}  // namespace wizard