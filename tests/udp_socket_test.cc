#include <gtest/gtest.h>
#include <maroof/net/UdpSocket.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/Error.hh>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

using maroof::net::UdpSocket;
using maroof::net::Endpoint;
using maroof::net::ErrorCode;

// Fixed high ports, one per test, to avoid conflicts across sequential runs.
// If a real service happens to hold one, the test fails loudly at bind.
constexpr std::uint16_t PORT_BIND       = 34561;
constexpr std::uint16_t PORT_NONBLOCK   = 34562;
constexpr std::uint16_t PORT_MOVE_CTOR  = 34563;
constexpr std::uint16_t PORT_MOVE_ASSIGN= 34564;
constexpr std::uint16_t PORT_ROUNDTRIP  = 34565;
constexpr std::uint16_t PORT_DOUBLE_BIND= 34566;

TEST(UdpSocketTest, ConstructsWithoutThrowing)
{
    EXPECT_NO_THROW({ UdpSocket sock; });
}

TEST(UdpSocketTest, BindToLoopbackSucceeds)
{
    UdpSocket sock;
    auto result = sock.bind(Endpoint("127.0.0.1", PORT_BIND));
    ASSERT_TRUE(result.has_value());
}

TEST(UdpSocketTest, DoubleBindFails)
{
    UdpSocket a;
    UdpSocket b;
    ASSERT_TRUE(a.bind(Endpoint("127.0.0.1", PORT_DOUBLE_BIND)).has_value());

    auto result = b.bind(Endpoint("127.0.0.1", PORT_DOUBLE_BIND));
    EXPECT_FALSE(result.has_value());
}

TEST(UdpSocketTest, SetNonblockingSucceeds)
{
    UdpSocket sock;
    auto result = sock.set_nonblocking();
    ASSERT_TRUE(result.has_value());
}

TEST(UdpSocketTest, NonBlockingRecvOnEmptyReturnsWouldBlock)
{
    UdpSocket sock;
    ASSERT_TRUE(sock.bind(Endpoint("127.0.0.1", PORT_NONBLOCK)).has_value());
    ASSERT_TRUE(sock.set_nonblocking().has_value());

    std::array<std::byte, 1024> buf {};
    auto result = sock.recv_from(std::span<std::byte>(buf));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::WouldBlock);
}

TEST(UdpSocketTest, MoveConstructorTransfersFd)
{
    UdpSocket a;
    UdpSocket b { std::move(a) };

    // b should own a valid fd — proven by successful bind.
    // a should be empty — its destructor should be a no-op (no double close).
    EXPECT_TRUE(b.bind(Endpoint("127.0.0.1", PORT_MOVE_CTOR)).has_value());
}

TEST(UdpSocketTest, MoveAssignmentTransfersFd)
{
    UdpSocket a;
    UdpSocket b;
    b = std::move(a);

    // b now owns a's fd; the fd it previously owned should have been closed.
    EXPECT_TRUE(b.bind(Endpoint("127.0.0.1", PORT_MOVE_ASSIGN)).has_value());
}

TEST(UdpSocketTest, LoopbackRoundTrip)
{
    UdpSocket server;
    UdpSocket client;
    ASSERT_TRUE(server.bind(Endpoint("127.0.0.1", PORT_ROUNDTRIP)).has_value());

    // Client is unbound — the OS will pick an ephemeral source port on first send.
    const char* msg = "hello";
    std::span<const std::byte> payload {
        reinterpret_cast<const std::byte*>(msg),
        5
    };

    auto sent = client.send_to(payload, Endpoint("127.0.0.1", PORT_ROUNDTRIP));
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(sent.value(), 5u);

    std::array<std::byte, 64> buf {};
    auto recv_result = server.recv_from(std::span<std::byte>(buf));
    ASSERT_TRUE(recv_result.has_value());

    const auto [bytes, peer] = recv_result.value();
    EXPECT_EQ(bytes, 5u);
    EXPECT_EQ(std::memcmp(buf.data(), msg, 5), 0);

    // Server should be able to echo back to the peer it just learned about.
    auto echo = server.send_to(std::span<const std::byte>(buf.data(), bytes), peer);
    ASSERT_TRUE(echo.has_value());
    EXPECT_EQ(echo.value(), 5u);
}
