#include <gtest/gtest.h>

#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UnreliableSequencedChannel.hh>
#include <maroof/net/UnreliableUnorderedChannel.hh>

#include <cstddef>
#include <cstring>
#include <span>

using namespace maroof::net;

namespace
{
    std::span<const std::byte> as_bytes(const char* s)
    {
        return { reinterpret_cast<const std::byte*>(s), std::strlen(s) };
    }
}

// ---------- UnreliableUnorderedChannel ----------

TEST(UnreliableUnorderedTx, SendProducesParseableDataPacketOnChannel1)
{
    UnreliableUnorderedChannel c;
    auto bytes = c.send(as_bytes("hi"));

    auto parsed = parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.type, PacketType::Data);
    EXPECT_EQ(parsed->first.channel_id, UnreliableUnorderedChannel::CHANNEL_ID);
    EXPECT_EQ(parsed->second.size(), 2u);
    EXPECT_EQ(std::memcmp(parsed->second.data(), "hi", 2), 0);
}

TEST(UnreliableUnorderedTx, SendIncrementsSequencePerCall)
{
    UnreliableUnorderedChannel c;

    auto seq1 = parse(c.send(as_bytes("a"))).value().first.sequence;
    auto seq2 = parse(c.send(as_bytes("b"))).value().first.sequence;

    EXPECT_EQ(seq2, seq1 + 1);
}

TEST(UnreliableUnorderedRx, DeliversEveryTimeRegardlessOfSequence)
{
    UnreliableUnorderedChannel c;
    int calls = 0;
    auto deliver = [&](std::span<const std::byte>) { calls++; };

    c.receive(0, as_bytes("a"),   deliver);
    c.receive(1, as_bytes("b"),   deliver);
    c.receive(0, as_bytes("dup"), deliver);   // duplicate seq — still delivers
    c.receive(5, as_bytes("gap"), deliver);   // out of order — still delivers
    c.receive(1, as_bytes("old"), deliver);   // stale seq — still delivers

    EXPECT_EQ(calls, 5);
}

// ---------- UnreliableSequencedChannel ----------

TEST(UnreliableSequencedTx, SendProducesParseableDataPacketOnChannel2)
{
    UnreliableSequencedChannel c;
    auto bytes = c.send(as_bytes("hi"));

    auto parsed = parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.type, PacketType::Data);
    EXPECT_EQ(parsed->first.channel_id, UnreliableSequencedChannel::CHANNEL_ID);
    EXPECT_EQ(parsed->second.size(), 2u);
    EXPECT_EQ(std::memcmp(parsed->second.data(), "hi", 2), 0);
}

TEST(UnreliableSequencedTx, SendIncrementsSequencePerCall)
{
    UnreliableSequencedChannel c;

    auto seq1 = parse(c.send(as_bytes("a"))).value().first.sequence;
    auto seq2 = parse(c.send(as_bytes("b"))).value().first.sequence;

    EXPECT_EQ(seq2, seq1 + 1);
}

TEST(UnreliableSequencedRx, FirstPacketDeliversRegardlessOfSequence)
{
    UnreliableSequencedChannel c;
    int calls = 0;
    auto deliver = [&](std::span<const std::byte>) { calls++; };

    // Arbitrary starting seq — lazy init means we don't assume 0.
    c.receive(42, as_bytes("hi"), deliver);
    EXPECT_EQ(calls, 1);
}

TEST(UnreliableSequencedRx, NewerSequencesDeliverInAnyOrder)
{
    UnreliableSequencedChannel c;
    int calls = 0;
    auto deliver = [&](std::span<const std::byte>) { calls++; };

    c.receive(1, as_bytes("a"), deliver);
    c.receive(2, as_bytes("b"), deliver);
    c.receive(3, as_bytes("c"), deliver);

    EXPECT_EQ(calls, 3);
}

TEST(UnreliableSequencedRx, StaleSequenceDropped)
{
    UnreliableSequencedChannel c;
    int calls = 0;
    auto deliver = [&](std::span<const std::byte>) { calls++; };

    c.receive(5, as_bytes("newer"), deliver);   // deliver, watermark = 5
    c.receive(3, as_bytes("older"), deliver);   // drop
    c.receive(4, as_bytes("older"), deliver);   // drop

    EXPECT_EQ(calls, 1);
}

TEST(UnreliableSequencedRx, DuplicateSequenceDropped)
{
    UnreliableSequencedChannel c;
    int calls = 0;
    auto deliver = [&](std::span<const std::byte>) { calls++; };

    c.receive(5, as_bytes("first"), deliver);
    c.receive(5, as_bytes("dup"),   deliver);   // drop — same seq is not strictly newer

    EXPECT_EQ(calls, 1);
}

TEST(UnreliableSequencedRx, GapAheadStillDelivers)
{
    UnreliableSequencedChannel c;
    int calls = 0;
    auto deliver = [&](std::span<const std::byte>) { calls++; };

    c.receive(5,  as_bytes("a"), deliver);   // deliver, watermark = 5
    c.receive(3,  as_bytes("b"), deliver);   // drop — stale
    c.receive(10, as_bytes("c"), deliver);   // deliver, watermark = 10 (jump is fine)

    EXPECT_EQ(calls, 2);
}

TEST(UnreliableSequencedRx, PayloadContentDeliveredCorrectly)
{
    UnreliableSequencedChannel c;
    std::vector<std::byte> last;
    auto deliver = [&](std::span<const std::byte> p) { last.assign(p.begin(), p.end()); };

    c.receive(1, as_bytes("hello"), deliver);
    ASSERT_EQ(last.size(), 5u);
    EXPECT_EQ(std::memcmp(last.data(), "hello", 5), 0);
}
