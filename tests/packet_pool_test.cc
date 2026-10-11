#include <gtest/gtest.h>

#include <maroof/net/PacketPool.hh>

#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>

using namespace maroof::net;

TEST(PacketPool, EmptyAtConstruction)
{
    PacketPool pool { 4 };
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_EQ(pool.slots_in_use(), 0u);
}

TEST(PacketPool, AcquireIncrementsReleaseDecrements)
{
    PacketPool pool { 4 };
    {
        auto a = pool.acquire(100);
        EXPECT_EQ(pool.slots_in_use(), 1u);
        {
            auto b = pool.acquire(200);
            EXPECT_EQ(pool.slots_in_use(), 2u);
        }
        EXPECT_EQ(pool.slots_in_use(), 1u);
    }
    EXPECT_EQ(pool.slots_in_use(), 0u);
}

TEST(PacketPool, ExhaustionThrowsBadAlloc)
{
    PacketPool pool { 2 };
    auto a = pool.acquire(10);
    auto b = pool.acquire(10);
    EXPECT_THROW(pool.acquire(10), std::bad_alloc);
}

TEST(PacketPool, OversizedAcquireThrowsLengthError)
{
    PacketPool pool { 4 };
    EXPECT_THROW(pool.acquire(PacketPool::SLOT_SIZE + 1), std::length_error);
}

TEST(PacketPool, ReleasedSlotIsReusable)
{
    PacketPool pool { 2 };
    {
        auto a = pool.acquire(10);
        auto b = pool.acquire(10);
    }
    EXPECT_NO_THROW({
        auto c = pool.acquire(10);
        auto d = pool.acquire(10);
    });
}

TEST(PacketPool, BufferIsWritableAtRequestedSize)
{
    PacketPool pool { 1 };
    auto buf = pool.acquire(100);
    ASSERT_EQ(buf.size(), 100u);
    ASSERT_EQ(buf.bytes().size(), 100u);

    buf.bytes()[0]  = std::byte { 0xAB };
    buf.bytes()[99] = std::byte { 0xCD };
    EXPECT_EQ(buf.bytes()[ 0], std::byte { 0xAB });
    EXPECT_EQ(buf.bytes()[99], std::byte { 0xCD });
}

TEST(PacketPool, MoveTransfersOwnership)
{
    PacketPool pool { 1 };
    auto a = pool.acquire(50);
    EXPECT_EQ(pool.slots_in_use(), 1u);

    PooledBuffer b { std::move(a) };
    EXPECT_EQ(pool.slots_in_use(), 1u);  // the slot stayed in use — ownership transferred
    EXPECT_TRUE(a.empty());
    EXPECT_FALSE(b.empty());
    EXPECT_EQ(b.size(), 50u);
}

TEST(PacketPool, MovedFromDoesNotDoubleRelease)
{
    PacketPool pool { 1 };
    {
        auto a = pool.acquire(50);
        PooledBuffer b { std::move(a) };
        // Both a and b fall out of scope — only b should return the slot.
    }
    EXPECT_EQ(pool.slots_in_use(), 0u);
    // If a double-release happened, this acquire would hand out a slot but push the
    // free list past capacity; capacity() tells us the physical slot count hasn't drifted.
    EXPECT_EQ(pool.capacity(), 1u);
}
