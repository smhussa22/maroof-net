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
#include <maroof/net/Connection.hh>
#include <maroof/net/UdpSocket.hh>

using namespace maroof::net;

// Client ---------------------------------------------------------------
UdpSocket socket;
socket.set_nonblocking().value();
Connection conn { Endpoint("10.0.0.5", 9000) };

// Handshake
auto hello = conn.make_hello();
std::array<std::byte, 2048> buf;
auto n = serialize(hello, {}, buf).value();
socket.send_to({buf.data(), n}, conn.peer());

// Send a reliable message. Returns one span per wire fragment.
auto wires = conn.send_reliable(payload);
for (auto bytes : wires) socket.send_to(bytes, conn.peer());

// Advance retransmit timers each loop iteration.
auto retransmits = conn.tick(std::chrono::steady_clock::now());
for (auto bytes : retransmits) socket.send_to(bytes, conn.peer());
```

Full working examples in `examples/`: `echo_client`, `echo_server` (epoll + batched I/O), `echo_server_uring` (io_uring), `flood_client` (reliability stress test), `latency_bench` (CO-safe benchmark harness).

## Build

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires GCC 14+ or Clang 18+ (C++23: `<print>`, `<expected>`, `<span>`). Linux-only (uses POSIX sockets, `epoll`, `io_uring`, `sched_setaffinity`).

Dependencies pulled in automatically: GoogleTest, HdrHistogram_c, liburing.

## Benchmarks

Numbers from WSL2 on commodity x86_64, both client and server pinned to separate cores. Loopback, no injected loss.

| Workload | p50 | p99 | p99.9 | p99.99 |
|---|---|---|---|---|
| 10K msg/s (epoll server) | 44 µs | 140 µs | 298 µs | 564 µs |
| 50K msg/s (epoll server) | 29 µs | 195 µs | 1.0 ms | — |
| 10K msg/s (io_uring server) | 38 µs | 173 µs | 1.1 ms | 4.2 ms |
| 50K msg/s (io_uring server) | 39 µs | 431 µs | 920 µs | 1.2 ms |

End-to-end send→ACK, including the full handshake, fragmentation, pending-map lookup, and histogram record. 100% of messages ACKed under all configurations.

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
