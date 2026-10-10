#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UdpSocket.hh>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

// Stress test: fires N reliable messages back-to-back at the echo server. Passes if every
// message gets ACKed within the deadline (send-path reliability). The return-path echoes
// come back unreliably, so their arrival is only reported, not required.

using namespace maroof::net;

namespace
{
    struct PolledPacket
    {
        PacketHeader header;
        std::span<const std::byte> payload;
    };

    std::optional<PolledPacket> poll_once(
        UdpSocket& socket,
        const Endpoint& expected_peer,
        std::span<std::byte> recv_buf)
    {

        auto received = socket.recv_from(recv_buf);
        if (!received.has_value()) return std::nullopt;

        const auto [bytes, peer] = received.value();
        if (!(peer == expected_peer)) return std::nullopt;

        auto parsed = parse(std::span<const std::byte>(recv_buf.data(), bytes));
        if (!parsed.has_value()) return std::nullopt;

        return PolledPacket { parsed.value().first, parsed.value().second };

    }
}

int main(int argc, char* argv[])
{

    const std::string_view server_addr { argc > 1 ? argv[1] : "127.0.0.1:9000" };
    std::uint32_t message_count { 1000 };
    if (argc > 2)
    {
        std::from_chars(argv[2], argv[2] + std::strlen(argv[2]), message_count);
    }

    UdpSocket socket;
    if (!socket.set_nonblocking().has_value())
    {
        std::println("set_nonblocking failed");
        return 1;
    }

    Endpoint   server { server_addr };
    Connection conn   { server };

    std::array<std::byte, 2048> send_buf {};
    std::array<std::byte, 2048> recv_buf {};

    // ---------------- Handshake (same pattern as echo_client) ----------------
    constexpr int  max_attempts   = 5;
    constexpr auto retry_interval = std::chrono::milliseconds(200);

    bool established = false;
    for (int attempt = 1; attempt <= max_attempts && !established; ++attempt)
    {

        auto hello = conn.make_hello();
        auto n = serialize(hello, {}, std::span<std::byte>(send_buf)).value();
        socket.send_to(std::span<const std::byte>(send_buf.data(), n), server).value();

        auto deadline = std::chrono::steady_clock::now() + retry_interval;
        while (std::chrono::steady_clock::now() < deadline && !established)
        {

            auto pkt = poll_once(socket, server, std::span<std::byte>(recv_buf));
            if (pkt && pkt->header.type == PacketType::HelloAck)
            {
                conn.on_hello_ack();
                established = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        }

    }

    if (!established)
    {
        std::println("handshake failed");
        return 1;
    }
    std::println("[handshake] established with {}", server.to_string());

    // ---------------- Flood: fire N reliable messages ----------------
    // Payload is the message index as an 8-byte little-endian value so the server echoes
    // the number back verbatim and we can identify which echo corresponds to which send.
    std::unordered_set<std::uint64_t> acked;
    std::unordered_set<std::uint64_t> echoed;
    std::uint32_t out_of_order_echoes = 0;
    std::uint32_t duplicate_echoes    = 0;
    std::uint64_t highest_echo_seen   = 0;

    auto flood_start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < message_count; ++i)
    {

        std::uint64_t payload_val = i;
        std::span<const std::byte> payload {
            reinterpret_cast<const std::byte*>(&payload_val),
            sizeof(payload_val)
        };
        auto wires = conn.send_reliable(payload);
        for (auto bytes : wires)
        {
            socket.send_to(bytes, server).value();
        }

    }
    std::println("[flood] queued {} reliable sends in {} ms",
                 message_count,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - flood_start).count());

    // ---------------- Drain: tick + poll until all acked or timeout ----------------
    auto overall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() < overall_deadline && acked.size() < message_count)
    {

        auto now = std::chrono::steady_clock::now();
        auto retransmits = conn.tick(now);
        for (auto bytes : retransmits)
        {
            socket.send_to(bytes, server).value();
        }

        if (conn.state() == Connection::State::Disconnected)
        {
            std::println("connection dead after {} acks", acked.size());
            return 1;
        }

        // Drain everything currently in the socket buffer before ticking again.
        while (auto pkt = poll_once(socket, server, std::span<std::byte>(recv_buf)))
        {

            // Any packet from the peer resets the heartbeat timer.
            conn.note_activity(std::chrono::steady_clock::now());

            if (pkt->header.type == PacketType::Ack)
            {
                conn.handle_ack(pkt->header.sequence);
                acked.insert(pkt->header.sequence);
            }
            else if (pkt->header.type == PacketType::Data && pkt->payload.size() == sizeof(std::uint64_t))
            {

                std::uint64_t val = 0;
                std::memcpy(&val, pkt->payload.data(), sizeof(val));

                if (!echoed.insert(val).second)
                {
                    ++duplicate_echoes;
                }
                else
                {
                    if (val < highest_echo_seen) ++out_of_order_echoes;
                    else highest_echo_seen = val;
                }

            }

        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - flood_start).count();

    std::println("");
    std::println("=== Flood Summary ===");
    std::println("messages sent            : {}", message_count);
    std::println("acks received            : {} / {}", acked.size(), message_count);
    std::println("echoes received (unreliable return path) : {}", echoed.size());
    std::println("echoes out-of-order      : {}", out_of_order_echoes);
    std::println("duplicate echoes         : {}", duplicate_echoes);
    std::println("elapsed                  : {} ms", elapsed_ms);

    if (acked.size() < message_count)
    {
        std::println("FAIL: {} messages never ACKed", message_count - acked.size());
        return 1;
    }

    std::println("PASS: all {} messages ACKed", message_count);
    return 0;

}
