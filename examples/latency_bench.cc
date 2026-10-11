#include <maroof/net/Connection.hh>
#include <maroof/net/CpuAffinity.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/LatencyHistogram.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UdpSocket.hh>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

// Fixed-rate latency benchmark. Sends N msgs/sec for D seconds, measures intended_send->ack
// (NOT actual_send->ack) so a stalled sender surfaces its missed slots at full latency —
// this is the coordinated-omission correction Gil Tene's talk is about.

using namespace maroof::net;
using namespace std::chrono_literals;

namespace
{
    struct Flags
    {
        std::string_view server { "127.0.0.1:9000" };
        std::uint32_t    rate     { 10'000 };   // messages per second
        std::uint32_t    duration { 10 };       // seconds
        int              cpu      { -1 };       // -1 = don't pin
    };

    Flags parse_flags(int argc, char* argv[])
    {
        Flags f;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg { argv[i] };
            if (arg == "--server" && i + 1 < argc)
            {
                f.server = argv[++i];
            }
            else if (arg == "--rate" && i + 1 < argc)
            {
                std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), f.rate);
                ++i;
            }
            else if (arg == "--duration" && i + 1 < argc)
            {
                std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), f.duration);
                ++i;
            }
            else if (arg == "--cpu" && i + 1 < argc)
            {
                std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), f.cpu);
                ++i;
            }
        }
        return f;
    }

    struct PolledPacket
    {
        PacketHeader header;
        std::span<const std::byte> payload;
    };

    std::optional<PolledPacket> poll_once(UdpSocket& socket,
                                          const Endpoint& peer,
                                          std::span<std::byte> buf)
    {
        auto received = socket.recv_from(buf);
        if (!received.has_value()) return std::nullopt;

        const auto [bytes, from] = received.value();
        if (!(from == peer)) return std::nullopt;

        auto parsed = parse(std::span<const std::byte>(buf.data(), bytes));
        if (!parsed.has_value()) return std::nullopt;
        return PolledPacket { parsed->first, parsed->second };
    }

    bool do_handshake(UdpSocket& socket,
                      Connection& conn,
                      const Endpoint& peer,
                      std::span<std::byte> send_buf,
                      std::span<std::byte> recv_buf)
    {
        for (int attempt = 1; attempt <= 5; ++attempt)
        {
            auto hello = conn.make_hello();
            auto n = serialize(hello, {}, send_buf).value();
            socket.send_to(std::span<const std::byte>(send_buf.data(), n), peer).value();

            auto deadline = std::chrono::steady_clock::now() + 200ms;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto pkt = poll_once(socket, peer, recv_buf);
                if (pkt && pkt->header.type == PacketType::HelloAck)
                {
                    conn.on_hello_ack();
                    return true;
                }
                std::this_thread::sleep_for(2ms);
            }
        }
        return false;
    }
}

int main(int argc, char* argv[])
{
    const auto flags = parse_flags(argc, argv);

    if (flags.cpu >= 0)
    {
        if (pin_current_thread_to(flags.cpu))
        {
            std::println("[cpu] pinned to core {}", flags.cpu);
        }
        else
        {
            std::println("[cpu] WARNING: failed to pin to core {} — continuing unpinned", flags.cpu);
        }
    }

    UdpSocket socket;
    if (!socket.set_nonblocking().has_value())
    {
        std::println("set_nonblocking failed");
        return 1;
    }

    Endpoint   server { flags.server };
    Connection conn   { server };

    std::array<std::byte, 2048> send_buf {};
    std::array<std::byte, 2048> recv_buf {};

    if (!do_handshake(socket, conn, server, send_buf, recv_buf))
    {
        std::println("handshake failed");
        return 1;
    }
    std::println("[handshake] established with {}", server.to_string());

    // --- Schedule ------------------------------------------------------------
    // intended[i] = t0 + i * interval. Short lead-in so the first few sends aren't crammed
    // against t0 while the loop is still winding up.
    const auto t0       = std::chrono::steady_clock::now() + 100ms;
    const auto interval = std::chrono::nanoseconds { 1'000'000'000 / flags.rate };
    const std::uint64_t total =
        static_cast<std::uint64_t>(flags.rate) * flags.duration;

    LatencyHistogram latency { 1ns, 60s };
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> intended;
    intended.reserve(total);

    std::uint64_t sent  = 0;
    std::uint64_t acked = 0;

    const auto drain_deadline = t0 + std::chrono::seconds { flags.duration } + 10s;

    while (acked < total && std::chrono::steady_clock::now() < drain_deadline)
    {
        const auto now = std::chrono::steady_clock::now();

        // Fire every message whose scheduled time has passed. Loop (not single-shot) so a
        // brief stall in the drain path still gets caught up — multiple messages can go out
        // in one iteration when we're behind.
        while (sent < total)
        {
            const auto scheduled = t0 + sent * interval;
            if (scheduled > now) break;

            std::uint64_t idx = sent;
            std::span<const std::byte> payload {
                reinterpret_cast<const std::byte*>(&idx), sizeof(idx)
            };

            auto wires = conn.send_reliable(payload);
            for (auto bytes : wires)
            {
                socket.send_to(bytes, server).value();
                auto parsed = parse(bytes).value();
                // Key by channel-assigned sequence; value is the SCHEDULED time (not now),
                // so a late send still gets its lateness counted on the ack side.
                intended.emplace(parsed.first.sequence, scheduled);
            }
            ++sent;
        }

        // Retransmits.
        auto retransmits = conn.tick(now);
        for (auto bytes : retransmits) socket.send_to(bytes, server).value();
        if (conn.state() == Connection::State::Disconnected)
        {
            std::println("connection dead; sent={} acked={}", sent, acked);
            return 1;
        }

        // Drain the socket of all currently-queued packets before looping back.
        while (auto pkt = poll_once(socket, server, std::span<std::byte>(recv_buf)))
        {
            conn.note_activity(std::chrono::steady_clock::now());

            if (pkt->header.type == PacketType::Ack)
            {
                auto it = intended.find(pkt->header.sequence);
                if (it != intended.end())
                {
                    const auto sample = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - it->second);
                    latency.record(sample);
                    conn.handle_ack(pkt->header.sequence);
                    intended.erase(it);
                    ++acked;
                }
            }
            // Data packets are echoes — we're measuring send->ack, not round-trip. Ignore.
        }
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::println("");
    std::println("=== Latency Bench ===");
    std::println("target rate     : {} msgs/sec", flags.rate);
    std::println("duration        : {} s",        flags.duration);
    std::println("messages sent   : {}",          sent);
    std::println("acked           : {} / {}",     acked, total);
    std::println("elapsed         : {} ms",       elapsed_ms);
    std::println("");
    std::println("intended->ack latency (CO-safe):");
    std::println("  p50     : {} ns",   latency.percentile( 50.0).count());
    std::println("  p99     : {} ns",   latency.percentile( 99.0).count());
    std::println("  p99.9   : {} ns",   latency.percentile( 99.9).count());
    std::println("  p99.99  : {} ns",   latency.percentile(99.99).count());

    return (acked == total) ? 0 : 1;
}
