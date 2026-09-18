#include <gtest/gtest.h>

#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>

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

    // Runs receive_reliable_data and captures each delivered payload as an
    // owned copy so the test can assert on it after the call returns.
    std::vector<std::vector<std::byte>> collect(Connection& c, std::uint64_t seq, std::span<const std::byte> payload)
    {
        std::vector<std::vector<std::byte>> out;
        c.receive_reliable_data(seq, payload, [&](std::span<const std::byte> p) {
            out.emplace_back(p.begin(), p.end());
        });
        return out;
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
    c.accept_hello();
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

TEST(ConnectionReliability, MakeReliableDataReturnsBytesWithHeaderAndPayload)
{
    Connection c { make_peer() };
    c.accept_hello();

    auto payload = as_bytes("hello");
    auto bytes = c.make_reliable_data(payload, 0);

    EXPECT_EQ(bytes.size(), PACKET_HEADER_WIRE_SIZE + payload.size());

    // Verify the produced bytes parse back to a Data packet with matching payload.
    auto parsed = parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.type, PacketType::Data);
    EXPECT_EQ(parsed->second.size(), payload.size());
    EXPECT_EQ(std::memcmp(parsed->second.data(), payload.data(), payload.size()), 0);
}

TEST(ConnectionReliability, TickBeforeTimeoutReturnsEmpty)
{
    Connection c { make_peer() };
    c.accept_hello();
    c.make_reliable_data(as_bytes("hi"), 0);

    // Timer hasn't fired yet.
    auto retransmits = c.tick(std::chrono::steady_clock::now());
    EXPECT_TRUE(retransmits.empty());
}

TEST(ConnectionReliability, TickAfterTimeoutReturnsRetransmit)
{
    Connection c { make_peer() };
    c.accept_hello();
    c.make_reliable_data(as_bytes("hi"), 0);

    // Advance well past INITIAL_TIMEOUT.
    auto future = std::chrono::steady_clock::now() + 1s;
    auto retransmits = c.tick(future);
    EXPECT_EQ(retransmits.size(), 1u);
}

TEST(ConnectionReliability, HandleAckStopsRetransmits)
{
    Connection c { make_peer() };
    c.accept_hello();
    c.make_reliable_data(as_bytes("hi"), 0);

    // Parse the produced bytes to find the sequence number the peer would ACK.
    auto bytes = c.make_reliable_data(as_bytes("second"), 0);
    auto parsed = parse(bytes).value();
    auto seq_of_second = parsed.first.sequence;

    c.handle_ack(seq_of_second);

    // Advance past timeout: only ONE retransmit (the first msg), not two.
    auto retransmits = c.tick(std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(retransmits.size(), 1u);
}

TEST(ConnectionReliability, HandleAckForUnknownSequenceIsNoOp)
{
    Connection c { make_peer() };
    c.accept_hello();
    // No pending packets.
    EXPECT_NO_THROW(c.handle_ack(999));
}

TEST(ConnectionReliability, ExhaustingRetriesDisconnectsPeer)
{
    Connection c { make_peer() };
    c.accept_hello();
    c.make_reliable_data(as_bytes("hi"), 0);

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
    c.accept_hello();

    // Sending an Ack shouldn't burn a sequence number - the next Data should
    // start where make_hello / accept_hello left off, not skip past it.
    auto first_data  = c.make_reliable_data(as_bytes("a"), 0);
    auto first_seq   = parse(first_data).value().first.sequence;

    c.make_ack(999);
    c.make_ack(1000);

    auto second_data = c.make_reliable_data(as_bytes("b"), 0);
    auto second_seq  = parse(second_data).value().first.sequence;

    EXPECT_EQ(second_seq, first_seq + 1);
}

TEST(ConnectionReliableRx, FirstSeenSetsWatermarkAndDelivers)
{
    Connection c { make_peer() };
    auto out = collect(c, 5, as_bytes("hello"));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(std::memcmp(out[0].data(), "hello", 5), 0);
}

TEST(ConnectionReliableRx, SequentialInOrderEachDelivers)
{
    Connection c { make_peer() };
    auto a = collect(c, 1, as_bytes("one"));
    auto b = collect(c, 2, as_bytes("two"));
    auto d = collect(c, 3, as_bytes("three"));

    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(std::memcmp(a[0].data(), "one",   3), 0);
    EXPECT_EQ(std::memcmp(b[0].data(), "two",   3), 0);
    EXPECT_EQ(std::memcmp(d[0].data(), "three", 5), 0);
}

TEST(ConnectionReliableRx, DuplicateOfDeliveredReturnsEmpty)
{
    Connection c { make_peer() };
    collect(c, 1, as_bytes("one"));

    auto dup = collect(c, 1, as_bytes("one"));
    EXPECT_TRUE(dup.empty());
}

TEST(ConnectionReliableRx, OutOfOrderStashesReturningEmpty)
{
    Connection c { make_peer() };
    collect(c, 1, as_bytes("one"));

    // seq 3 arrives before seq 2 - stashed, not delivered yet.
    auto stashed = collect(c, 3, as_bytes("three"));
    EXPECT_TRUE(stashed.empty());
}

TEST(ConnectionReliableRx, GapFillDrainsInOrder)
{
    Connection c { make_peer() };
    collect(c, 1, as_bytes("one"));

    collect(c, 3, as_bytes("three"));  // stash
    collect(c, 4, as_bytes("four"));   // stash

    // seq 2 fills the gap - should deliver 2, then drain 3 and 4.
    auto out = collect(c, 2, as_bytes("two"));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(std::memcmp(out[0].data(), "two",   3), 0);
    EXPECT_EQ(std::memcmp(out[1].data(), "three", 5), 0);
    EXPECT_EQ(std::memcmp(out[2].data(), "four",  4), 0);
}

TEST(ConnectionReliableRx, DuplicateOfStashedReturnsEmpty)
{
    Connection c { make_peer() };
    collect(c, 1, as_bytes("one"));

    collect(c, 3, as_bytes("three"));            // stash
    auto dup = collect(c, 3, as_bytes("three")); // duplicate of stashed
    EXPECT_TRUE(dup.empty());
}

TEST(ConnectionReliableRx, PayloadIsCopiedNotAliased)
{
    Connection c { make_peer() };
    collect(c, 1, as_bytes("one"));

    std::vector<std::byte> buf(5);
    std::memcpy(buf.data(), "three", 5);
    collect(c, 3, std::span<const std::byte>(buf));

    // Overwrite the caller's buffer to prove we didn't alias it.
    std::memcpy(buf.data(), "XXXXX", 5);

    auto out = collect(c, 2, as_bytes("two"));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(std::memcmp(out[1].data(), "three", 5), 0);
}

TEST(ConnectionReliableRx, BeyondWindowDropped)
{
    Connection c { make_peer() };
    collect(c, 0, as_bytes("first"));

    // Way past the window front (window is 128 slots).
    auto out = collect(c, 500, as_bytes("far"));
    EXPECT_TRUE(out.empty());
}
