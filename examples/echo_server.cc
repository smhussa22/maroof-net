#include <maroof/net/UdpSocket.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/Error.hh>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <string_view>
#include <system_error>

int main(int argc, char* argv[])
{

    std::uint16_t port { 9000 };
    if (argc > 1)
    {

        const std::string_view arg { argv[1] };
        std::from_chars(arg.data(), arg.data() + arg.size(), port);

    }

    maroof::net::UdpSocket socket;

    if (auto r { socket.bind(maroof::net::Endpoint("0.0.0.0", port)) }; !r.has_value())
    {

        std::println("bind failed");
        return 1;

    }

    std::println("echo server listening on 0.0.0.0:{}", port);
    std::array<std::byte, 2048> buf {};

    for (;;)
    {

        auto received = socket.recv_from(std::span<std::byte>(buf));
        if (!received.has_value())
        {

            std::println("recv_from error");
            continue;

        }

        const auto [bytes, peer] = received.value();
        std::println("received {} bytes from {}", bytes, peer.to_string());

        auto sent = socket.send_to(std::span<const std::byte>(buf.data(), bytes), peer);
        if (!sent.has_value())
        {

            std::println("send_to error");
            continue;
            
        }

    }

    return 0;

}
