#include <gtest/gtest.h>

#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/ReliableOrderedChannel.hh>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

using namespace maroof::net;

namespace
{
    std::span<const std::byte> as_bytes(const char* s)
    {
        return { reinterpret_cast<const std::byte*>(s), std::strlen(s) };
    }

    PacketHeader data_header(std::uint64_t seq,
                             std::uint16_t frag_id    = 0,
                             std::uint16_t frag_count = 1)
    {
        PacketHeader h { seq, PacketType::Data, ReliableOrderedChannel::CHANNEL_ID, 0 };
        h.fragment_id    = frag_id;
        h.fragment_count = frag_count;
        return h;
    }

    // Runs channel.receive and captures each delivered payload as an owned copy
    // so the test can assert on it after the call returns.
    std::vector<std::vector<std::byte>> collect(ReliableOrderedChannel& ch,
                                                std::uint64_t seq,
                                                std::span<const std::byte> payload)
    {
        std::vector<std::vector<std::byte>> out;
        ch.receive(data_header(seq), payload, [&](std::span<const std::byte> p) {
            out.emplace_back(p.begin(), p.end());
        });
        return out;
    }

    std::vector<std::vector<std::byte>> collect_fragment(ReliableOrderedChannel& ch,
                                                         std::uint64_t seq,
                                                         std::uint16_t frag_id,
                                                         std::uint16_t frag_count,
                                                         std::span<const std::byte> payload)
    {
        std::vector<std::vector<std::byte>> out;
        ch.receive(data_header(seq, frag_id, frag_count), payload, [&](std::span<const std::byte> p) {
            out.emplace_back(p.begin(), p.end());
        });
        return out;
    }
}

TEST(ReliableOrderedRx, FirstSeenSetsWatermarkAndDelivers)
{
    ReliableOrderedChannel ch;
    auto out = collect(ch, 5, as_bytes("hello"));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(std::memcmp(out[0].data(), "hello", 5), 0);
}

TEST(ReliableOrderedRx, SequentialInOrderEachDelivers)
{
    ReliableOrderedChannel ch;
    auto a = collect(ch, 1, as_bytes("one"));
    auto b = collect(ch, 2, as_bytes("two"));
    auto d = collect(ch, 3, as_bytes("three"));

    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(std::memcmp(a[0].data(), "one",   3), 0);
    EXPECT_EQ(std::memcmp(b[0].data(), "two",   3), 0);
    EXPECT_EQ(std::memcmp(d[0].data(), "three", 5), 0);
}

TEST(ReliableOrderedRx, DuplicateOfDeliveredReturnsEmpty)
{
    ReliableOrderedChannel ch;
    collect(ch, 1, as_bytes("one"));

    auto dup = collect(ch, 1, as_bytes("one"));
    EXPECT_TRUE(dup.empty());
}

TEST(ReliableOrderedRx, OutOfOrderStashesReturningEmpty)
{
    ReliableOrderedChannel ch;
    collect(ch, 1, as_bytes("one"));

    // seq 3 arrives before seq 2 — stashed, not delivered yet.
    auto stashed = collect(ch, 3, as_bytes("three"));
    EXPECT_TRUE(stashed.empty());
}

TEST(ReliableOrderedRx, GapFillDrainsInOrder)
{
    ReliableOrderedChannel ch;
    collect(ch, 1, as_bytes("one"));

    collect(ch, 3, as_bytes("three"));  // stash
    collect(ch, 4, as_bytes("four"));   // stash

    // seq 2 fills the gap — should deliver 2, then drain 3 and 4.
    auto out = collect(ch, 2, as_bytes("two"));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(std::memcmp(out[0].data(), "two",   3), 0);
    EXPECT_EQ(std::memcmp(out[1].data(), "three", 5), 0);
    EXPECT_EQ(std::memcmp(out[2].data(), "four",  4), 0);
}

TEST(ReliableOrderedRx, DuplicateOfStashedReturnsEmpty)
{
    ReliableOrderedChannel ch;
    collect(ch, 1, as_bytes("one"));

    collect(ch, 3, as_bytes("three"));            // stash
    auto dup = collect(ch, 3, as_bytes("three")); // duplicate of stashed
    EXPECT_TRUE(dup.empty());
}

TEST(ReliableOrderedRx, PayloadIsCopiedNotAliased)
{
    ReliableOrderedChannel ch;
    collect(ch, 1, as_bytes("one"));

    std::vector<std::byte> buf(5);
    std::memcpy(buf.data(), "three", 5);
    collect(ch, 3, std::span<const std::byte>(buf));

    // Overwrite the caller's buffer to prove we didn't alias it.
    std::memcpy(buf.data(), "XXXXX", 5);

    auto out = collect(ch, 2, as_bytes("two"));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(std::memcmp(out[1].data(), "three", 5), 0);
}

TEST(ReliableOrderedRx, BeyondWindowReturnsFalse)
{
    ReliableOrderedChannel ch;
    collect(ch, 0, as_bytes("first"));

    // Way past the window front (window is 128 slots). Should return false so the
    // sender keeps retrying instead of getting a permanent hole.
    bool accepted = ch.receive(data_header(500), as_bytes("far"), [](std::span<const std::byte>) {});
    EXPECT_FALSE(accepted);
}

TEST(ReliableOrderedRx, BelowWindowReturnsTrue)
{
    ReliableOrderedChannel ch;
    collect(ch, 5, as_bytes("first"));   // watermark = 6

    // seq 3 is below the window (already delivered as far as we're concerned) —
    // we should re-ACK because our earlier ACK was probably lost.
    bool accepted = ch.receive(data_header(3), as_bytes("stale"), [](std::span<const std::byte>) {});
    EXPECT_TRUE(accepted);
}

TEST(ReliableOrderedRx, SeedReceiveIsIdempotent)
{
    ReliableOrderedChannel ch;
    ch.seed_receive(10);

    // First delivered seq should be 10, not 20 — second seed_receive is a no-op.
    ch.seed_receive(20);
    auto out = collect(ch, 10, as_bytes("first"));
    ASSERT_EQ(out.size(), 1u);
}

// -------- Adaptive RTO --------

TEST(ReliableOrderedRto, NoRttFallsBackToInitialTimeout)
{
    ReliableOrderedChannel ch;
    ch.send(as_bytes("hi"));  // no RTT supplied

    // Tick just before the 100ms fallback — nothing should fire.
    auto now = std::chrono::steady_clock::now();
    auto retransmits = ch.tick(now + std::chrono::milliseconds(50));
    EXPECT_TRUE(retransmits.empty());
}

TEST(ReliableOrderedRto, RttDrivenDeadlineFiresEarlierThanFallback)
{
    using namespace std::chrono_literals;

    ReliableOrderedChannel ch;
    // 10ms RTT → base = 15ms → deadline ~15ms from send. Tick at 50ms should fire.
    ch.send(as_bytes("hi"), 10ms);

    auto now = std::chrono::steady_clock::now();
    auto retransmits = ch.tick(now + 50ms, 10ms);
    EXPECT_EQ(retransmits.size(), 1u);
}

TEST(ReliableOrderedRto, SubMillisecondRttFloorsAtMinRto)
{
    using namespace std::chrono_literals;

    ReliableOrderedChannel ch;
    // 100µs RTT * 1.5 = 150µs, below MIN_RTO (20ms). Tick at 10ms must NOT fire — floor holds.
    ch.send(as_bytes("hi"), 100us);

    auto now = std::chrono::steady_clock::now();
    auto retransmits = ch.tick(now + 10ms, 100us);
    EXPECT_TRUE(retransmits.empty());
}

// -------- AIMD --------

TEST(ReliableOrderedAimd, SendIncrementsInFlight)
{
    ReliableOrderedChannel ch;
    EXPECT_EQ(ch.in_flight(), 0u);
    ch.send(as_bytes("a"));
    ch.send(as_bytes("b"));
    EXPECT_EQ(ch.in_flight(), 2u);
}

TEST(ReliableOrderedAimd, AckDecrementsInFlightAndGrowsCwnd)
{
    ReliableOrderedChannel ch;
    const auto start_cwnd = ch.cwnd();

    ch.send(as_bytes("a"));
    ch.handle_ack(0);

    EXPECT_EQ(ch.in_flight(), 0u);
    EXPECT_EQ(ch.cwnd(), start_cwnd + 1u);
}

TEST(ReliableOrderedAimd, DuplicateAckDoesNotGrowCwndFurther)
{
    ReliableOrderedChannel ch;
    ch.send(as_bytes("a"));
    ch.handle_ack(0);
    const auto cwnd_after_first = ch.cwnd();

    ch.handle_ack(0);  // duplicate — the packet is already gone from m_pending
    EXPECT_EQ(ch.cwnd(), cwnd_after_first);
}

TEST(ReliableOrderedAimd, RetransmitHalvesCwnd)
{
    using namespace std::chrono_literals;

    ReliableOrderedChannel ch;
    const auto start_cwnd = ch.cwnd();

    ch.send(as_bytes("hi"));

    // Tick well past the fallback deadline to force a retransmit.
    auto now = std::chrono::steady_clock::now();
    auto retransmits = ch.tick(now + 1s);
    ASSERT_EQ(retransmits.size(), 1u);
    EXPECT_EQ(ch.cwnd(), std::max<std::uint32_t>(1, start_cwnd / 2));
}

// -------- Fragmentation --------

TEST(ReliableOrderedFragmentation, SmallPayloadIsSingleFragment)
{
    ReliableOrderedChannel ch;
    auto wires = ch.send(as_bytes("hi"));
    ASSERT_EQ(wires.size(), 1u);

    auto parsed = parse(wires[0]);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.fragment_id,    0u);
    EXPECT_EQ(parsed->first.fragment_count, 1u);
}

TEST(ReliableOrderedFragmentation, LargePayloadSplitsIntoMultipleFragments)
{
    ReliableOrderedChannel ch;

    // 3000 bytes with MAX_FRAGMENT_PAYLOAD=1200 → ceil(3000/1200) = 3 fragments.
    std::vector<std::byte> big(3000);
    for (std::size_t i = 0; i < big.size(); ++i)
    {
        big[i] = static_cast<std::byte>(i & 0xFF);
    }

    auto wires = ch.send(std::span<const std::byte>(big));
    ASSERT_EQ(wires.size(), 3u);

    // Each fragment carries fragment_count=3 and fragment_id 0..2 and consecutive sequences.
    std::uint64_t prev_seq = 0;
    for (std::size_t i = 0; i < wires.size(); ++i)
    {
        auto parsed = parse(wires[i]);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->first.fragment_id,    i);
        EXPECT_EQ(parsed->first.fragment_count, 3u);
        if (i == 0) prev_seq = parsed->first.sequence;
        else        EXPECT_EQ(parsed->first.sequence, prev_seq + i);
    }
}

TEST(ReliableOrderedFragmentation, ReceiveReassemblesInOrderFragments)
{
    ReliableOrderedChannel ch;

    // Simulated incoming fragments: three slices of a 7-byte message.
    auto a = as_bytes("abc");
    auto b = as_bytes("de");
    auto c = as_bytes("fg");

    EXPECT_TRUE(collect_fragment(ch, 0, 0, 3, a).empty());  // mid-message, no delivery yet
    EXPECT_TRUE(collect_fragment(ch, 1, 1, 3, b).empty());
    auto out = collect_fragment(ch, 2, 2, 3, c);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].size(), 7u);
    EXPECT_EQ(std::memcmp(out[0].data(), "abcdefg", 7), 0);
}

TEST(ReliableOrderedFragmentation, ReceiveReassemblesOutOfOrderFragments)
{
    ReliableOrderedChannel ch;
    ch.seed_receive(0);  // in production, accept_hello seeds the window to 0

    // Fragments arrive 2, 0, 1 — window stashes them and drains in order on the gap-fill.
    auto a = as_bytes("abc");
    auto b = as_bytes("de");
    auto c = as_bytes("fg");

    EXPECT_TRUE(collect_fragment(ch, 2, 2, 3, c).empty());  // stashed
    EXPECT_TRUE(collect_fragment(ch, 0, 0, 3, a).empty());  // front; waits for 1 before draining 2
    auto out = collect_fragment(ch, 1, 1, 3, b);            // drains 1 then 2 → delivers

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].size(), 7u);
    EXPECT_EQ(std::memcmp(out[0].data(), "abcdefg", 7), 0);
}

TEST(ReliableOrderedFragmentation, FragmentsAreIndependentlyRetransmitted)
{
    using namespace std::chrono_literals;

    ReliableOrderedChannel ch;

    std::vector<std::byte> big(2500);  // → 3 fragments
    auto wires = ch.send(std::span<const std::byte>(big));
    ASSERT_EQ(wires.size(), 3u);
    EXPECT_EQ(ch.in_flight(), 3u);

    // ACK the middle fragment only — the other two should still retransmit on timeout.
    auto parsed_middle = parse(wires[1]);
    ASSERT_TRUE(parsed_middle.has_value());
    ch.handle_ack(parsed_middle->first.sequence);
    EXPECT_EQ(ch.in_flight(), 2u);

    auto retransmits = ch.tick(std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(retransmits.size(), 2u);
}
