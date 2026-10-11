# maroof::net

A reliable-UDP transport library in modern C++23. Connection-oriented, message-oriented, with per-message reliability semantics — a dropped unreliable packet never stalls a reliable one. Shape is similar to ENet, GameNetworkingSockets, and Aeron.

## Features

**Transport**
- Three channel types per connection, multiplexed on one UDP socket:
  - Reliable + ordered (TCP-shaped, with its own sliding-window dedup/reorder)
  - Unreliable + unordered (fire-and-forget)
  - Unreliable + sequenced (latest-wins; stale datagrams dropped)
- Handshake (`Hello` / `HelloAck`), heartbeat death detection, graceful disconnect
- MTU-aware fragmentation + in-order reassembly on the reliable channel
- Adaptive RTO from an EWMA-smoothed RTT (`Ping`/`Pong`), with exponential backoff and a floor
- AIMD congestion window (per-ACK additive increase, per-retransmit multiplicative decrease)
- CRC32 over the full header + payload; type-safe `PacketHeader` serialize/parse
- UDP multicast (`join_multicast_group`, `MCAST_JOIN_SOURCE_GROUP` for SSM) + `SO_REUSEPORT` for multi-threaded fanout
- Piggybacked ACKs — the Connection stashes the pending ACK and the next outbound send on any channel attaches it to its header automatically. Stand-alone Ack packets only fire when the receiver has nothing to say back.

**Performance**
- Zero-allocation `send()` hot path via a fixed-size `PacketPool` + RAII `PooledBuffer`
- O(log N) retransmit timer via a `multimap`-backed min-heap (vs the naive O(N) scan)
- Batched I/O (`recvmmsg` / `sendmmsg`) on `UdpSocket` — one syscall per 64 datagrams
- Two reactor implementations: `EpollReactor` (readiness-based) and `IoUringReactor` (completion-based async via liburing)
- Lock-free `SpscQueue<T, N>` for one-producer / one-consumer handoff between threads (cache-line-aligned head/tail, standard SPSC memory ordering)
- CPU affinity helper (`pin_current_thread_to`) for pinning the calling thread

**Measurement**
- `LatencyHistogram` wrapping HdrHistogram_c; records samples with 3-sigfig precision across nanoseconds-to-minutes
- Send→ACK latency recorded per-ACK inside `ReliableOrderedChannel`
- `latency_bench` fixed-rate harness: schedules N msgs/sec, measures *intended-send → ack* so coordinated omission is corrected

## Example

```cpp
#include <maroof/net/Client.hh>
#include <maroof/net/Server.hh>
using namespace maroof::net;

// Server ---------------------------------------------------------------
Server server { Endpoint("0.0.0.0", 9000) };
server
    .on_connect   ([](Connection& c) { /* new peer */ })
    .on_receive   ([](Connection& c, std::span<const std::byte> p, std::uint8_t ch) {
        // handle message on channel ch
    })
    .on_disconnect([](const Endpoint& peer) { /* peer went away */ });
server.run();   // or server.tick() per frame

// Client ---------------------------------------------------------------
Client client { Endpoint("10.0.0.5", 9000) };
client
    .on_connect([&] { client.send_reliable(my_payload); })
    .on_receive([](std::span<const std::byte> p, std::uint8_t ch) { /* reply */ });
client.connect();
while (true) client.tick();
```

Hand-driven alternatives (for callers who want the IO loop under their own control): `UdpSocket` + `Connection` directly. See `examples/echo_server.cc` for the explicit pattern.

Full examples in `examples/`:
- `simple_server` / `simple_client` — the API shown above
- `echo_server` — hand-driven `epoll` + `recvmmsg` + `sendmmsg`
- `echo_server_uring` — hand-driven `io_uring`
- `flood_client` — 1000-message reliability stress test
- `latency_bench` — CO-safe fixed-rate benchmark harness

## Build

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires GCC 14+ or Clang 18+ (C++23: `<print>`, `<expected>`, `<span>`). Linux-only (uses POSIX sockets, `epoll`, `io_uring`, `sched_setaffinity`).

Dependencies pulled in automatically: GoogleTest, HdrHistogram_c, liburing.

## Benchmarks

Release build, pinned to isolated cores on WSL2 x86_64. Reproduce with:

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target perf_summary -j
taskset -c 2,3 ./build-rel/perf_summary
```

| Operation | Throughput | Latency |
|---|---|---|
| Serialize packet (28 B header + 64 B payload, CRC32) | ~5M ops/sec | 186 ns |
| Parse packet (same, CRC verified) | ~5M ops/sec | 185 ns |
| Pool acquire + release | ~238M ops/sec | 4.2 ns |
| SPSC push + pop (uncontended) | ~761M ops/sec | 1.3 ns |
| SPSC handoff (producer + consumer on separate cores) | ~285M ops/sec | 3.5 ns |

End-to-end send→ACK over loopback with `examples/latency_bench.cc` (coordinated-omission-safe, median of 5 runs):

| Load | p50 | p99 | p99.9 |
|---|---|---|---|
| 10K msg/s | 37 µs | 186 µs | 308 µs |
| 50K msg/s | 30 µs | 294 µs | 585 µs |

Reproduce:
```bash
# UDP library
./echo_server       9000 --cpu 3 --quiet &
./latency_bench --rate 10000 --duration 5 --cpu 2

# TCP baseline
./tcp_echo_server   9500 --cpu 3 &
./tcp_latency_bench --server 127.0.0.1 --port 9500 --rate 10000 --duration 5 --cpu 2
```

## Non-goals

- No TLS/DTLS. Layer encryption on top if needed.
- No NAT traversal.
- Not a TCP replacement — TCP is fine where TCP is fine.

## Layout

```
include/maroof/net/   Public headers
src/net/              Implementations
examples/             Demos: echo client/server, flood, latency bench, io_uring server
tests/                GoogleTest unit + integration tests (100+ tests)
```

Namespace: everything lives under `maroof::net`. Top-level `maroof::` is reserved for future utilities.
