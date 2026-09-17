#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

#include "message.h"

namespace wizard {

// Unix stream socket wrapper for the engine/gateway protocol.
class UnixSocket {
public:
    ~UnixSocket();

    UnixSocket(UnixSocket&& other) noexcept;
    UnixSocket& operator=(UnixSocket&& other) noexcept;
    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;

    // Sends a message. Returns false on write failure.
    bool send(const Message& msg);

    // Reads available bytes and returns all complete messages parsed.
    // Returns nullopt if the peer disconnected or on read error.
    std::optional<std::vector<Message>> receive();

    bool is_valid() const { return fd_ >= 0; }

private:
    explicit UnixSocket(int fd);
    friend UnixSocket connect_to(const std::string& path);
    friend std::optional<UnixSocket> listen_and_accept(const std::string& path);

    int fd_;
    MessageParser parser_;
};

// Server side: bind, listen, block until one client connects.
std::optional<UnixSocket> listen_and_accept(const std::string& path);

// Client side: connect to a listening socket. Throws on failure.
UnixSocket connect_to(const std::string& path);

}  // namespace wizard