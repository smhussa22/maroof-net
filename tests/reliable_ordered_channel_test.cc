#include <gtest/gtest.h>

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

    // Runs channel.receive and captures each delivered payload as an owned copy
    // so the test can assert on it after the call returns.
    std::vector<std::vector<std::byte>> collect(ReliableOrderedChannel& ch,
                                                std::uint64_t seq,
                                                std::span<const std::byte> payload)
    {
        std::vector<std::vector<std::byte>> out;
        ch.receive(seq, payload, [&](std::span<const std::byte> p) {
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
    bool accepted = ch.receive(500, as_bytes("far"), [](std::span<const std::byte>) {});
    EXPECT_FALSE(accepted);
}

TEST(ReliableOrderedRx, BelowWindowReturnsTrue)
{
    ReliableOrderedChannel ch;
    collect(ch, 5, as_bytes("first"));   // watermark = 6

    // seq 3 is below the window (already delivered as far as we're concerned) —
    // we should re-ACK because our earlier ACK was probably lost.
    bool accepted = ch.receive(3, as_bytes("stale"), [](std::span<const std::byte>) {});
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
