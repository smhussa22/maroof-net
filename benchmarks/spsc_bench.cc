#include <benchmark/benchmark.h>

#include <maroof/net/SpscQueue.hh>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

// A/B micro-bench: lock-free SpscQueue vs. the obvious mutex-protected std::queue baseline.
// Both pass N u64 items from a producer thread to a consumer thread; we time the full handoff.

using namespace maroof::net;

static constexpr std::uint64_t ITEMS = 1'000'000;

static void BM_SpscQueue(benchmark::State& state)
{
    for (auto _ : state)
    {
        SpscQueue<std::uint64_t, 4096> q;

        std::thread producer([&] {
            for (std::uint64_t i = 0; i < ITEMS; ++i)
            {
                while (!q.try_push(i)) { /* spin */ }
            }
        });

        std::uint64_t last_seen = 0;
        std::uint64_t received  = 0;
        while (received < ITEMS)
        {
            if (auto v = q.try_pop())
            {
                last_seen = *v;
                ++received;
            }
        }
        producer.join();
        benchmark::DoNotOptimize(last_seen);
    }
    state.SetItemsProcessed(state.iterations() * ITEMS);
}
BENCHMARK(BM_SpscQueue)->Unit(benchmark::kMillisecond)->Iterations(5);

static void BM_MutexQueue(benchmark::State& state)
{
    for (auto _ : state)
    {
        std::queue<std::uint64_t> q;
        std::mutex                m;

        std::thread producer([&] {
            for (std::uint64_t i = 0; i < ITEMS; ++i)
            {
                std::lock_guard lk { m };
                q.push(i);
            }
        });

        std::uint64_t last_seen = 0;
        std::uint64_t received  = 0;
        while (received < ITEMS)
        {
            std::uint64_t v;
            {
                std::lock_guard lk { m };
                if (q.empty()) continue;
                v = q.front();
                q.pop();
            }
            last_seen = v;
            ++received;
        }
        producer.join();
        benchmark::DoNotOptimize(last_seen);
    }
    state.SetItemsProcessed(state.iterations() * ITEMS);
}
BENCHMARK(BM_MutexQueue)->Unit(benchmark::kMillisecond)->Iterations(5);

BENCHMARK_MAIN();
