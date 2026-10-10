#include <gtest/gtest.h>

#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/ReliableOrderedChannel.hh>
#include <maroof/net/UnreliableSequencedChannel.hh>
#include <maroof/net/UnreliableUnorderedChannel.hh>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>

using namespace maroof::net;
using namespace std::chrono_literals;

namespace
{
    Endpoint make_peer() { return Endpoint("127.0.0.1", 55555); }

    std::span<const std::byte> as_bytes(const char* s)
    {
        return { reinterpret_cast<const std::byte*>(s), std::strlen(s) };
    }
}

TEST(ConnectionHandshake, MakeHelloTransitionsToConnecting)
{
    Connection c { make_peer() };
    EXPECT_EQ(c.state(), Connection::State::Disconnected);
    c.make_hello();
    EXPECT_EQ(c.state(), Connection::State::Connecting);
}

TEST(ConnectionHandshake, AcceptHelloTransitionsToEstablished)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    EXPECT_EQ(c.state(), Connection::State::Established);
}

TEST(ConnectionHandshake, OnHelloAckMovesConnectingToEstablished)
{
    Connection c { make_peer() };
    c.make_hello();
    c.on_hello_ack();
    EXPECT_EQ(c.state(), Connection::State::Established);
}

TEST(ConnectionHandshake, OnHelloAckIsNoOpFromDisconnected)
{
    Connection c { make_peer() };
    c.on_hello_ack();  // stray - no prior Hello
    EXPECT_EQ(c.state(), Connection::State::Disconnected);
}

TEST(ConnectionReliability, SendReliableReturnsBytesWithHeaderAndPayload)
{
    Connection c { make_peer() };
    c.accept_hello(0);

    auto payload = as_bytes("hello");
    auto wires = c.send_reliable(payload);
    ASSERT_EQ(wires.size(), 1u);

    EXPECT_EQ(wires[0].size(), PACKET_HEADER_WIRE_SIZE + payload.size());

    auto parsed = parse(wires[0]);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.type, PacketType::Data);
    EXPECT_EQ(parsed->first.channel_id, 0);
    EXPECT_EQ(parsed->second.size(), payload.size());
    EXPECT_EQ(std::memcmp(parsed->second.data(), payload.data(), payload.size()), 0);
}

TEST(ConnectionReliability, TickBeforeTimeoutReturnsEmpty)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    c.send_reliable(as_bytes("hi"));

    // Timer hasn't fired yet.
    auto retransmits = c.tick(std::chrono::steady_clock::now());
    EXPECT_TRUE(retransmits.empty());
}

TEST(ConnectionReliability, TickAfterTimeoutReturnsRetransmit)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    c.send_reliable(as_bytes("hi"));

    // Advance well past INITIAL_TIMEOUT.
    auto future = std::chrono::steady_clock::now() + 1s;
    auto retransmits = c.tick(future);
    EXPECT_EQ(retransmits.size(), 1u);
}

TEST(ConnectionReliability, HandleAckStopsRetransmits)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    c.send_reliable(as_bytes("hi"));

    // Parse the produced bytes to find the sequence number the peer would ACK.
    auto wires = c.send_reliable(as_bytes("second"));
    ASSERT_EQ(wires.size(), 1u);
    auto parsed = parse(wires[0]).value();
    auto seq_of_second = parsed.first.sequence;

    c.handle_ack(seq_of_second);

    // Advance past timeout: only ONE retransmit (the first msg), not two.
    auto retransmits = c.tick(std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(retransmits.size(), 1u);
}

TEST(ConnectionReliability, HandleAckForUnknownSequenceIsNoOp)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    // No pending packets.
    EXPECT_NO_THROW(c.handle_ack(999));
}

TEST(ConnectionReliability, ExhaustingRetriesDisconnectsPeer)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    c.send_reliable(as_bytes("hi"));

    // Each tick must be past the updated retransmit deadline (exponential backoff).
    // Using hours(i+1) ensures every tick fires regardless of backoff magnitude.
    for (int i = 0; i < 20; ++i)
    {
        c.tick(std::chrono::steady_clock::now() + std::chrono::hours(i + 1));
        if (c.state() == Connection::State::Disconnected) break;
    }

    EXPECT_EQ(c.state(), Connection::State::Disconnected);
}

TEST(ConnectionReliability, MakeAckCarriesReceivedSequence)
{
    Connection c { make_peer() };
    auto ack = c.make_ack(42);
    EXPECT_EQ(ack.type, PacketType::Ack);
    EXPECT_EQ(ack.sequence, 42u);
}

TEST(ConnectionReliability, MakeAckDoesNotConsumeSequenceCounter)
{
    Connection c { make_peer() };
    c.accept_hello(0);

    // Sending an Ack shouldn't burn a sequence number - the next Data should
    // start where the previous send left off, not skip past it.
    auto first_wires  = c.send_reliable(as_bytes("a"));
    ASSERT_EQ(first_wires.size(), 1u);
    auto first_seq    = parse(first_wires[0]).value().first.sequence;

    c.make_ack(999);
    c.make_ack(1000);

    auto second_wires = c.send_reliable(as_bytes("b"));
    ASSERT_EQ(second_wires.size(), 1u);
    auto second_seq   = parse(second_wires[0]).value().first.sequence;

    EXPECT_EQ(second_seq, first_seq + 1);
}

// Sliding-window receive tests moved to reliable_ordered_channel_test.cc — they belong to the
// channel unit, not Connection. Connection-level dispatch is covered below via receive_data.

TEST(ConnectionDispatch, ReceiveDataRoutesToReliableChannelAndSignalsAck)
{
    Connection c { make_peer() };
    c.accept_hello(0);

    // Channel 0 packet at expected seq 0 (channels always seed to 0 on accept_hello).
    PacketHeader h { 0, PacketType::Data, ReliableOrderedChannel::CHANNEL_ID, 0 };
    bool delivered = false;
    bool should_ack = c.receive_data(h, as_bytes("hi"), [&](std::span<const std::byte>) { delivered = true; });

    EXPECT_TRUE(delivered);
    EXPECT_TRUE(should_ack);
}

TEST(ConnectionDispatch, ReceiveDataOnUnreliableChannelNeverAcks)
{
    Connection c { make_peer() };
    c.accept_hello(0);

    PacketHeader h { 42, PacketType::Data, UnreliableUnorderedChannel::CHANNEL_ID, 0 };
    bool delivered = false;
    bool should_ack = c.receive_data(h, as_bytes("hi"), [&](std::span<const std::byte>) { delivered = true; });

    EXPECT_TRUE(delivered);
    EXPECT_FALSE(should_ack);
}

TEST(ConnectionDispatch, ReceiveDataOnSequencedChannelNeverAcks)
{
    Connection c { make_peer() };
    c.accept_hello(0);

    PacketHeader h { 1, PacketType::Data, UnreliableSequencedChannel::CHANNEL_ID, 0 };
    bool delivered = false;
    bool should_ack = c.receive_data(h, as_bytes("hi"), [&](std::span<const std::byte>) { delivered = true; });

    EXPECT_TRUE(delivered);
    EXPECT_FALSE(should_ack);
}

TEST(ConnectionDispatch, ReceiveDataOnUnknownChannelDropsSilently)
{
    Connection c { make_peer() };
    c.accept_hello(0);

    PacketHeader h { 1, PacketType::Data, 99, 0 };  // no such channel
    bool delivered = false;
    bool should_ack = c.receive_data(h, as_bytes("hi"), [&](std::span<const std::byte>) { delivered = true; });

    EXPECT_FALSE(delivered);
    EXPECT_FALSE(should_ack);
}

TEST(ConnectionHeartbeat, MakePingReturnsPingHeaderWithIncrementingSequence)
{
    Connection c { make_peer() };
    auto now = std::chrono::steady_clock::now();

    auto p0 = c.make_ping(now);
    auto p1 = c.make_ping(now);

    EXPECT_EQ(p0.type, PacketType::Ping);
    EXPECT_EQ(p0.sequence, 0u);
    EXPECT_EQ(p1.sequence, 1u);
}

TEST(ConnectionHeartbeat, OnPingEchoesSequenceAsPong)
{
    Connection c { make_peer() };
    PacketHeader ping { 42, PacketType::Ping, 0, 0 };

    auto pong = c.on_ping(ping);

    EXPECT_EQ(pong.type, PacketType::Pong);
    EXPECT_EQ(pong.sequence, 42u);
}

TEST(ConnectionHeartbeat, OnPongUpdatesRtt)
{
    Connection c { make_peer() };
    auto t0 = std::chrono::steady_clock::now();

    auto ping = c.make_ping(t0);
    PacketHeader pong { ping.sequence, PacketType::Pong, 0, 0 };
    c.on_pong(pong, t0 + 50ms);

    ASSERT_TRUE(c.rtt().has_value());
    EXPECT_EQ(*c.rtt(), std::chrono::nanoseconds(50ms));
}

TEST(ConnectionHeartbeat, OnPongIgnoresUnknownSequence)
{
    Connection c { make_peer() };
    PacketHeader pong { 999, PacketType::Pong, 0, 0 };

    c.on_pong(pong, std::chrono::steady_clock::now());

    EXPECT_FALSE(c.rtt().has_value());
}

TEST(ConnectionHeartbeat, TickDisconnectsAfterHeartbeatDeath)
{
    Connection c { make_peer() };
    c.accept_hello(0);
    auto t0 = std::chrono::steady_clock::now();
    c.note_activity(t0);

    // Well past HEARTBEAT_DEATH (5s).
    c.tick(t0 + 10s);

    EXPECT_EQ(c.state(), Connection::State::Disconnected);
}
