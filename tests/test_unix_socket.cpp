#include <gtest/gtest.h>

#include <unistd.h>

#include <thread>

#include "unix_socket.h"

using namespace wizard;

namespace {
std::string test_socket_path() {
    return "/tmp/wizard_test_" + std::to_string(getpid()) + ".sock";
}
}  // namespace

TEST(UnixSocketTest, ClientServerExchangePingPong) {
    auto path = test_socket_path();

    std::optional<UnixSocket> server_sock;
    std::thread server_thread([&] { server_sock = listen_and_accept(path); });

    // Give the server a moment to bind+listen before the client connects.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    UnixSocket client = connect_to(path);
    server_thread.join();

    ASSERT_TRUE(server_sock.has_value());
    UnixSocket& server = *server_sock;

    Message ping{MessageType::Ping, {}};
    ASSERT_TRUE(client.send(ping));

    auto received = server.receive();
    ASSERT_TRUE(received.has_value());
    ASSERT_EQ(received->size(), 1u);
    EXPECT_EQ((*received)[0], ping);

    Message pong{MessageType::Pong, {}};
    ASSERT_TRUE(server.send(pong));

    auto reply = client.receive();
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply->size(), 1u);
    EXPECT_EQ((*reply)[0], pong);

    ::unlink(path.c_str());
}

TEST(UnixSocketTest, ReceiveReturnsNulloptAfterPeerCloses) {
    auto path = test_socket_path();

    std::optional<UnixSocket> server_sock;
    std::thread server_thread([&] { server_sock = listen_and_accept(path); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        UnixSocket client = connect_to(path);
        // client goes out of scope here -> socket closed
    }
    server_thread.join();
    ASSERT_TRUE(server_sock.has_value());

    // Give the OS a moment to deliver the close.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto received = server_sock->receive();
    EXPECT_FALSE(received.has_value());

    ::unlink(path.c_str());
}

TEST(UnixSocketTest, ConnectToMissingSocketThrows) {
    EXPECT_THROW(connect_to("/tmp/wizard_does_not_exist.sock"), std::runtime_error);
}