#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UdpSocket.hh>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <string_view>
#include <unordered_map>

using namespace maroof::net;

int main(int argc, char* argv[])
{

    std::uint16_t port { 9000 };
    if (argc > 1)
    {

        const std::string_view arg { argv[1] };
        std::from_chars(arg.data(), arg.data() + arg.size(), port);

    }

    UdpSocket socket;
    if (auto r { socket.bind(Endpoint("0.0.0.0", port)) }; !r.has_value())
    {
        std::println("bind failed");
        return 1;
    }

    std::println("echo server listening on 0.0.0.0:{}", port);

    std::unordered_map<Endpoint, Connection> connections;

    std::array<std::byte, 2048> recv_buf {};
    std::array<std::byte, 2048> send_buf {};

    for (;;)
    {

        auto received = socket.recv_from(std::span<std::byte>(recv_buf));
        if (!received.has_value())
        {
            continue;
        }

        const auto [bytes, peer] = received.value();

        auto parsed = parse(std::span<const std::byte>(recv_buf.data(), bytes));
        if (!parsed.has_value())
        {
            std::println("dropped malformed packet from {}", peer.to_string());
            continue;
        }

        const auto& [header, payload] = parsed.value();

        auto it = connections.find(peer);

        // Hello: idempotent handshake. Create the Connection if new, or re-send
        // HelloAck if it's a duplicate Hello (our previous ack was probably lost).
        if (header.type == PacketType::Hello)
        {

            if (it == connections.end())
            {
                it = connections.emplace(peer, Connection(peer)).first;
            }

            auto ack = it->second.accept_hello(header.sequence);
            if (auto n { serialize(ack, {}, std::span<std::byte>(send_buf)) }; n.has_value())
            {
                socket.send_to(std::span<const std::byte>(send_buf.data(), n.value()), peer).value();
                std::println("[handshake] accepted from {}", peer.to_string());
            }
            continue;

        }

        // Any non-Hello packet from an unknown or non-Established peer is dropped.
        if (it == connections.end() || it->second.state() != Connection::State::Established)
        {
            continue;
        }

        it->second.note_received(header.sequence);

        if (header.type == PacketType::Data)
        {

            // Dedup + reorder. Lambda fires once per in-order delivered payload.
            // Only ACK if the packet was accounted for — beyond-window packets must NOT be ACKed,
            // or the sender removes them from pending and they become a permanent hole.
            bool accepted = it->second.receive_reliable_data(header.sequence, payload,
                [&](std::span<const std::byte> p)
                {
                    auto reply = it->second.make_data_header(header.channel_id);
                    if (auto n { serialize(reply, p, std::span<std::byte>(send_buf)) }; n.has_value())
                    {
                        socket.send_to(std::span<const std::byte>(send_buf.data(), n.value()), peer).value();
                        std::println("[echo] {} bytes seq {} from {}", p.size(), header.sequence, peer.to_string());
                    }
                });

            if (accepted)
            {
                auto ack = it->second.make_ack(header.sequence);
                if (auto n { serialize(ack, {}, std::span<std::byte>(send_buf)) }; n.has_value())
                {
                    socket.send_to(std::span<const std::byte>(send_buf.data(), n.value()), peer).value();
                }
            }

        }

        // Ack packets from client (in response to our echoes) are silently dropped
        // for now - the server doesn't track its own sends as reliable in this demo.

    }

    return 0;

}
