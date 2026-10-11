#include <maroof/net/Client.hh>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>

// Minimal client using the Client class — the hand-driven version is echo_client.cc.

using namespace maroof::net;
using namespace std::chrono_literals;

int main(int argc, char* argv[])
{
    const std::string_view message { argc > 1 ? argv[1] : "hello" };
    const std::string_view server_addr { argc > 2 ? argv[2] : "127.0.0.1:9000" };

    Client client { Endpoint(server_addr) };

    bool got_echo = false;

    client
        .on_connect([&] {
            std::println("[connected]");
            std::span<const std::byte> payload {
                reinterpret_cast<const std::byte*>(message.data()), message.size()
            };
            client.send_reliable(payload);
            std::println("[sent] {} bytes reliable", message.size());
        })
        .on_receive([&](std::span<const std::byte> p, std::uint8_t channel) {
            const std::string s { reinterpret_cast<const char*>(p.data()), p.size() };
            std::println("[recv] channel {}: \"{}\"", channel, s);
            got_echo = true;
        })
        .on_disconnect([] {
            std::println("[disconnected]");
        });

    client.connect();

    // Run until echo arrives or we hit a 2s cap.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!got_echo && std::chrono::steady_clock::now() < deadline)
    {
        client.tick();
        std::this_thread::sleep_for(1ms);
    }

    const auto& stats = client.connection().stats();
    std::println("[stats] retransmits={}, dup_acks={}", stats.retransmits, stats.duplicate_acks_ignored);

    return got_echo ? 0 : 1;
}
