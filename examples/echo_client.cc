#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UdpSocket.hh>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>

using namespace maroof::net;

namespace
{
    struct PolledPacket
    {
        PacketHeader header;
        std::span<const std::byte> payload;
    };

    // Poll the socket until a packet arrives from expected_peer or the deadline
    // passes. Sleeps briefly between polls to avoid burning CPU. Returns the
    // parsed packet or std::nullopt on timeout.
    std::optional<PolledPacket> wait_for_packet_from(
        UdpSocket& socket,
        const Endpoint& expected_peer,
        std::span<std::byte> recv_buf,
        std::chrono::steady_clock::time_point deadline)
    {

        while (std::chrono::steady_clock::now() < deadline)
        {

            auto received = socket.recv_from(recv_buf);
            if (!received.has_value())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const auto [bytes, peer] = received.value();
            if (!(peer == expected_peer))
            {
                continue;
            }

            auto parsed = parse(std::span<const std::byte>(recv_buf.data(), bytes));
            if (!parsed.has_value())
            {
                continue;
            }

            return PolledPacket { parsed.value().first, parsed.value().second };

        }

        return std::nullopt;

    }
}

int main(int argc, char* argv[])
{

    const std::string_view message     { argc > 1 ? argv[1] : "hello"           };
    const std::string_view server_addr { argc > 2 ? argv[2] : "127.0.0.1:9000"  };

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

    // Handshake: send Hello, wait up to retry_interval for HelloAck, retry a
    // few times if it gets dropped along the way.
    constexpr int  max_attempts   = 5;
    constexpr auto retry_interval = std::chrono::milliseconds(200);

    bool established = false;
    for (int attempt = 1; attempt <= max_attempts && !established; ++attempt)
    {

        auto hello = conn.make_hello();
        auto n = serialize(hello, {}, std::span<std::byte>(send_buf)).value();
        if (!socket.send_to(std::span<const std::byte>(send_buf.data(), n), server).has_value())
        {
            std::println("send Hello failed");
            return 1;
        }
        std::println("[handshake] sent Hello #{}", attempt);

        auto deadline = std::chrono::steady_clock::now() + retry_interval;
        while (auto pkt = wait_for_packet_from(socket, server, std::span<std::byte>(recv_buf), deadline))
        {
            if (pkt->header.type == PacketType::HelloAck)
            {
                conn.on_hello_ack();
                established = true;
                std::println("[handshake] established with {}", server.to_string());
                break;
            }
        }

    }

    if (!established)
    {
        std::println("handshake failed after {} attempts", max_attempts);
        return 1;
    }

    // Handshake done. Send one Data packet and wait for the echoed Data back.
    auto data_header = conn.make_data_header(0);
    std::span<const std::byte> payload {
        reinterpret_cast<const std::byte*>(message.data()),
        message.size()
    };

    auto n = serialize(data_header, payload, std::span<std::byte>(send_buf)).value();
    if (!socket.send_to(std::span<const std::byte>(send_buf.data(), n), server).has_value())
    {
        std::println("send Data failed");
        return 1;
    }
    std::println("sent {} bytes seq {}: \"{}\"", message.size(), data_header.sequence, message);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (auto pkt = wait_for_packet_from(socket, server, std::span<std::byte>(recv_buf), deadline))
    {
        if (pkt->header.type == PacketType::Data)
        {
            conn.note_received(pkt->header.sequence);
            const std::string reply { reinterpret_cast<const char*>(pkt->payload.data()), pkt->payload.size() };
            std::println("received echo seq {}: \"{}\"", pkt->header.sequence, reply);
            return 0;
        }
    }

    std::println("timeout waiting for echo");
    return 1;

}
