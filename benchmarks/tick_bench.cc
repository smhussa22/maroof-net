#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <unordered_map>

// A/B micro-bench for the retransmit timer. Simulates ReliableOrderedChannel::tick()'s hot
// question: "given N unacked packets, how fast can I find and advance only the ones that are due?"
//
// LinearScan   — walks every entry in the unordered_map, checks deadline. O(N) per call.
// MinHeap      — multimap<deadline, seq> sorted by deadline; peek top, process while <= now.
//
// Two shapes exercised:
//   /K1  — only 1 packet is due per tick (the realistic case; ACKs usually arrive before RTO).
//   /K10 — 10% of pending packets are due simultaneously (loss-storm case).

using Clock = std::chrono::steady_clock;

struct Entry
{
    Clock::time_point deadline;
};

static void BM_Tick_LinearScan(benchmark::State& state)
{
    const std::size_t total = static_cast<std::size_t>(state.range(0));
    const std::size_t pct   = static_cast<std::size_t>(state.range(1));  // % of pending that is due

    const std::size_t due = (pct == 0) ? 1 : (total * pct / 100);
    const auto now  = Clock::now();

    for (auto _ : state)
    {
        state.PauseTiming();
        std::unordered_map<std::uint64_t, Entry> pending;
        pending.reserve(total);
        for (std::size_t i = 0; i < total; ++i)
        {
            pending[i] = { (i < due) ? now - std::chrono::milliseconds { 1 }
                                     : now + std::chrono::seconds { 10 } };
        }
        state.ResumeTiming();

        std::size_t processed = 0;
        for (auto& [seq, e] : pending)
        {
            if (e.deadline <= now)
            {
                ++processed;
                e.deadline = now + std::chrono::seconds { 10 };
            }
        }
        benchmark::DoNotOptimize(processed);
    }
    state.SetItemsProcessed(state.iterations() * due);
}
BENCHMARK(BM_Tick_LinearScan)
    ->ArgsProduct({ {32, 256, 2048, 8192}, {0, 10} });   // pct=0 → due=1; pct=10 → due=N/10

static void BM_Tick_MinHeap(benchmark::State& state)
{
    const std::size_t total = static_cast<std::size_t>(state.range(0));
    const std::size_t pct   = static_cast<std::size_t>(state.range(1));

    const std::size_t due = (pct == 0) ? 1 : (total * pct / 100);
    const auto now = Clock::now();

    for (auto _ : state)
    {
        state.PauseTiming();
        std::multimap<Clock::time_point, std::uint64_t> heap;
        for (std::size_t i = 0; i < total; ++i)
        {
            heap.emplace((i < due) ? now - std::chrono::milliseconds { 1 }
                                   : now + std::chrono::seconds { 10 },
                         static_cast<std::uint64_t>(i));
        }
        state.ResumeTiming();

        std::size_t processed = 0;
        while (!heap.empty() && heap.begin()->first <= now)
        {
            auto node = heap.extract(heap.begin());
            ++processed;
            node.key() = now + std::chrono::seconds { 10 };
            heap.insert(std::move(node));
        }
        benchmark::DoNotOptimize(processed);
    }
    state.SetItemsProcessed(state.iterations() * due);
}
BENCHMARK(BM_Tick_MinHeap)
    ->ArgsProduct({ {32, 256, 2048, 8192}, {0, 10} });

BENCHMARK_MAIN();
