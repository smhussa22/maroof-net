#include <maroof/net/Server.hh>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <string_view>

// Minimal server using the Server class — the hand-driven version is echo_server.cc.

using namespace maroof::net;

int main(int argc, char* argv[])
{
    std::uint16_t port { 9000 };
    if (argc > 1)
    {
        const std::string_view arg { argv[1] };
        std::from_chars(arg.data(), arg.data() + arg.size(), port);
    }

    Server server { Endpoint("0.0.0.0", port) };

    server
        .on_connect([](Connection& c) {
            std::println("[connect] {}", c.peer().to_string());
        })
        .on_disconnect([](const Endpoint& peer) {
            std::println("[disconnect] {}", peer.to_string());
        })
        .on_receive([&server](Connection& conn, std::span<const std::byte> payload, std::uint8_t channel) {
            std::println("[recv] {} bytes on channel {} from {}",
                         payload.size(), channel, conn.peer().to_string());
            // Echo back on the unreliable channel.
            auto reply = conn.send_unreliable(payload);
            (void) server.socket().send_to(reply, conn.peer());
        });

    std::println("simple_server listening on 0.0.0.0:{}", port);
    server.run();
    return 0;
}
