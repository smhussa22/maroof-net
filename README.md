# maroof::net

A reliable-UDP transport library in modern C++23. General-purpose networking library with per-message reliability semantics usable for anything that needs low-latency messaging over UDP. Inspired by things like ENet, GameNetworkingSockets, and Aeron.

This README describes the **end state**: what the library and its API look like once complete.

## What it is

A connection-oriented, message-oriented library over UDP. Callers pick reliability semantics **per message**, so a dropped unreliable packet never stalls a reliable one (no head-of-line blocking, unlike TCP). Same shape as ENet, GameNetworkingSockets, or Aeron.

## Public API (end-of-project shape)

```cpp
#include <maroof/net/Connection.hh>
#include <maroof/net/Server.hh>
#include <maroof/net/Subscriber.hh>

using namespace maroof::net;

// ----- Client -----
auto conn = Connection::connect(Endpoint("10.0.0.5", 9000)).value();

conn.on_receive([](std::span<const std::byte> payload, ChannelId ch) {
    // handle incoming message
});

conn.send_reliable  (bytes, /*channel=*/0);  // ordered + guaranteed — mini-TCP
conn.send_unreliable(bytes, /*channel=*/1);  // fire and forget — raw UDP
conn.send_sequenced (bytes, /*channel=*/2);  // latest-wins, stale dropped

// ----- Server -----
auto server = Server::bind(Endpoint("0.0.0.0", 9000)).value();
server.on_connect([](Connection& c) { /* new peer */ });
server.run();

// ----- Multicast (one-way subscribe, e.g. market feed) -----
auto sub = Subscriber::join(Endpoint("239.1.2.3", 30000)).value();
sub.on_receive([](std::span<const std::byte> payload) {
    // decode message
});
```

Errors returned as `std::expected<T, ErrorCode>`. Exceptions only for construction failures.

## Design highlights

- Multi-channel multiplexing on a single connection — no head-of-line blocking.
- Zero-allocation hot path — object pools for packet buffers, span-based APIs.
- `epoll` / `io_uring` event loop; blocking loop only in the earliest phases.
- Lock-free SPSC queue between the network thread and the application thread.
- Peer-agnostic packet header — multicast subscribes work without an API change.
- CPU pinning + cache-warm design; NUMA awareness where it matters.
- Batched syscalls (`recvmmsg` / `sendmmsg`) on the hot path.
- Optional Source-Specific Multicast (SSM) for the A/B arbitration pattern real feeds use.

## Use cases

The library is general-purpose. Concrete targets:

- **Market data feed handlers** — MoldUDP64-style publisher/receiver, sequence-gap detection, in-memory order book. This is the flagship demo (see Phase 5 in `goals.md`).
- **Real-time game networking** — same shape as ENet/GNS; per-message reliability avoids TCP's head-of-line blocking for latency-sensitive traffic.
- **Distributed system messaging** — Aeron-style publisher/subscriber over UDP.
- **Any low-latency message-oriented protocol** where TCP's stream semantics or head-of-line behavior are a problem.

## Benchmarks

*Populated at Phase 4. Placeholder for now.*

- Throughput: TBD msgs/sec on TBD hardware.
- p99.9 latency: TBD µs at TBD msgs/sec.
- Tail latency vs raw TCP under 0% / 1% / 5% loss and injected jitter: TBD.

## Build

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires GCC 14+ or Clang 18+ for C++23 library features (`<print>`, `<expected>`, etc.).

Run the Phase 0 echo demo:

```bash
./scripts/run_echo_demo.sh "hello world"
```

## What this project deliberately is NOT

- Not a TLS/DTLS layer — no built-in encryption. Callers who need it can layer it on.
- Not a NAT-traversal library — not the target use case.
- Not a TCP replacement — TCP is fine for what TCP is fine for.
- Not tied to any specific application domain.

## Namespace layout

- `maroof::net::` — everything network-related lives here.
- `maroof::` — reserved for future non-net utilities in the same repo.
- Public headers under `include/maroof/net/`.
