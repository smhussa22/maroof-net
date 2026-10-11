#include <gtest/gtest.h>

#include <maroof/net/SpscQueue.hh>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using namespace maroof::net;

TEST(SpscQueue, EmptyAtConstruction)
{
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());
    EXPECT_EQ(q.approx_size(), 0u);
    EXPECT_FALSE(q.try_pop().has_value());
}

TEST(SpscQueue, PushPopSingleThreadFifo)
{
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));

    auto a = q.try_pop();
    auto b = q.try_pop();
    auto c = q.try_pop();
    auto d = q.try_pop();

    ASSERT_TRUE(a.has_value()); EXPECT_EQ(*a, 1);
    ASSERT_TRUE(b.has_value()); EXPECT_EQ(*b, 2);
    ASSERT_TRUE(c.has_value()); EXPECT_EQ(*c, 3);
    EXPECT_FALSE(d.has_value());
}

TEST(SpscQueue, FillToCapacityThenRejects)
{
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(0));
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));   // full
    EXPECT_TRUE(q.full());
}

TEST(SpscQueue, WrapAroundPreservesFifo)
{
    SpscQueue<int, 4> q;
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(q.try_push(i));
    for (int i = 0; i < 3; ++i)
    {
        auto v = q.try_pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
    // Now push past the physical index boundary. Capacity = 4; head/tail have moved to 3.
    for (int i = 100; i < 104; ++i) EXPECT_TRUE(q.try_push(i));
    for (int i = 100; i < 104; ++i)
    {
        auto v = q.try_pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
}

// --- Threaded correctness ------------------------------------------------------------------

TEST(SpscQueue, TwoThreadsAllItemsArriveInOrder)
{
    constexpr std::size_t   CAPACITY = 1024;
    constexpr std::uint64_t TOTAL    = 1'000'000;

    SpscQueue<std::uint64_t, CAPACITY> q;
    std::vector<std::uint64_t>         received;
    received.reserve(TOTAL);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < TOTAL; ++i)
        {
            // Spin until push succeeds — a real producer would back off or batch; this is a test.
            while (!q.try_push(i)) { /* retry */ }
        }
    });

    std::thread consumer([&] {
        while (received.size() < TOTAL)
        {
            if (auto v = q.try_pop()) received.push_back(*v);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), TOTAL);
    for (std::uint64_t i = 0; i < TOTAL; ++i)
    {
        ASSERT_EQ(received[i], i) << "mismatch at index " << i;
    }
}
