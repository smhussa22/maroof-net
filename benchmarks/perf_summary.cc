#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketPool.hh>
#include <maroof/net/SpscQueue.hh>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

// Simple hand-rolled perf summary. Measures a handful of hot-path ops, divides, prints a
// table. Equivalent to running google-benchmark's results through sed/awk but one binary.
//
// Methodology: each op is run N times in a tight loop; we time the whole loop with
// steady_clock, then: latency_ns = total_ns / N; throughput = N / total_sec.
//
// Compile Release and pin to one core for stable numbers:
//   cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-rel --target perf_summary -j
//   taskset -c 2 ./build-rel/perf_summary

using namespace maroof::net;
using Clock = std::chrono::steady_clock;

struct Row { const char* op; double ns_per_op; double ops_per_sec; };

static Row bench(const char* op, std::uint64_t iters, auto&& body)
{
    const auto t0 = Clock::now();
    body(iters);
    const auto t1 = Clock::now();
    const double ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double ns_per_op = ns_total / static_cast<double>(iters);
    const double ops_per_sec = 1e9 / ns_per_op;
    return { op, ns_per_op, ops_per_sec };
}

static std::string humanize(double ops_per_sec)
{
    char buf[32];
    if      (ops_per_sec >= 1e9) std::snprintf(buf, sizeof(buf), "~%.1fB ops/sec", ops_per_sec / 1e9);
    else if (ops_per_sec >= 1e6) std::snprintf(buf, sizeof(buf), "~%.0fM ops/sec", ops_per_sec / 1e6);
    else if (ops_per_sec >= 1e3) std::snprintf(buf, sizeof(buf), "~%.0fK ops/sec", ops_per_sec / 1e3);
    else                         std::snprintf(buf, sizeof(buf), "~%.0f ops/sec",  ops_per_sec);
    return buf;
}

static std::string ns(double v)
{
    char buf[32];
    if (v < 10)   std::snprintf(buf, sizeof(buf), "%.1fns", v);
    else          std::snprintf(buf, sizeof(buf), "%.0fns", v);
    return buf;
}

int main()
{
    std::vector<Row> rows;

    // ---- Serialize / parse ------------------------------------------------
    {
        PacketHeader h { 42, PacketType::Data, 0, 0 };
        std::array<std::byte, 2048> buf {};
        std::array<std::byte, 64>   payload {};
        rows.push_back(bench("Serialize packet", 10'000'000, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                h.sequence = i;
                (void) serialize(h, std::span<const std::byte>(payload), std::span<std::byte>(buf));
            }
        }));

        // Prepare a valid serialized packet, then benchmark parse() on it in a loop.
        const auto written = serialize(h, std::span<const std::byte>(payload), std::span<std::byte>(buf)).value();
        rows.push_back(bench("Parse packet",      10'000'000, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                auto r = parse(std::span<const std::byte>(buf.data(), written));
                asm volatile ("" : : "r,m"(r) : "memory");  // keep the parse result alive
            }
        }));
    }

    // ---- Pool acquire + release ------------------------------------------
    {
        PacketPool pool { 8192 };
        rows.push_back(bench("Pool acquire+release", 100'000'000, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                auto b = pool.acquire(100);
                asm volatile ("" : : "r,m"(b) : "memory");
            }
        }));
    }

    // ---- SPSC queue push+pop (same thread; measures pure cost, no contention) --
    {
        SpscQueue<std::uint64_t, 1024> q;
        rows.push_back(bench("SPSC push+pop (same-thread)", 100'000'000, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                (void) q.try_push(i);
                auto v = q.try_pop();
                asm volatile ("" : : "r,m"(v) : "memory");
            }
        }));
    }

    // ---- SPSC queue throughput, two threads (producer + consumer) --------
    {
        SpscQueue<std::uint64_t, 4096> q;
        constexpr std::uint64_t N = 10'000'000;
        const auto t0 = Clock::now();
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < N; ++i)
                while (!q.try_push(i)) { }
        });
        std::uint64_t last = 0, got = 0;
        while (got < N) {
            if (auto v = q.try_pop()) { last = *v; ++got; }
        }
        producer.join();
        const auto t1 = Clock::now();
        const double ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        rows.push_back({ "SPSC handoff (2 threads)",
                         ns_total / static_cast<double>(N),
                         1e9 / (ns_total / static_cast<double>(N)) });
        (void) last;
    }

    // ---- Render -----------------------------------------------------------
    std::printf("\n");
    std::printf("Performance (Release build, pinned)\n");
    std::printf("====================================\n");
    std::printf("%-32s %-18s %-10s\n", "Operation", "Throughput", "Latency");
    std::printf("%-32s %-18s %-10s\n", "----------------------", "----------", "-------");
    for (const auto& r : rows)
    {
        std::printf("%-32s %-18s %-10s\n", r.op, humanize(r.ops_per_sec).c_str(), ns(r.ns_per_op).c_str());
    }
    std::printf("\n");
}
