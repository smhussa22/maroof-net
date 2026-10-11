#include <benchmark/benchmark.h>

#include <maroof/net/PacketPool.hh>

#include <cstddef>
#include <vector>

// A/B micro-bench: PacketPool.acquire() + implicit release vs. a fresh std::vector<std::byte>(size).
// Both deliver "give me a size-byte buffer I own." The vector version calls operator new[] every
// iteration; the pool version does a free-list pop + a small object construction.

using namespace maroof::net;

static void BM_HeapAlloc_Vector(benchmark::State& state)
{
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    for (auto _ : state)
    {
        std::vector<std::byte> buf(size);
        benchmark::DoNotOptimize(buf.data());  // prevent the compiler from eliding the allocation
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapAlloc_Vector)->Arg(28)->Arg(100)->Arg(1200);

static void BM_PoolAcquire(benchmark::State& state)
{
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    PacketPool pool { 1024 };
    for (auto _ : state)
    {
        auto buf = pool.acquire(size);   // acquire + implicit release when `buf` falls out of scope
        benchmark::DoNotOptimize(buf.bytes().data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolAcquire)->Arg(28)->Arg(100)->Arg(1200);

BENCHMARK_MAIN();
