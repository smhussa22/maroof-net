#include <maroof/net/UdpSocket.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/Error.hh>

#include <array>
#include <cstddef>
#include <print>
#include <span>
#include <string>
#include <string_view>

int main(int argc, char* argv[])
{

    const std::string_view message { argc > 1 ? argv[1] : "hello" };
    const std::string_view server_addr { argc > 2 ? argv[2] : "127.0.0.1:9000" };

    maroof::net::UdpSocket socket;
    maroof::net::Endpoint server { server_addr };

    std::span<const std::byte> payload 
    {
    
        reinterpret_cast<const std::byte*>(message.data()),
        message.size()
    
    };

    auto sent { socket.send_to(payload, server) };
    if (!sent.has_value())
    {

        std::println("send_to failed");
        return 1;
    
    }
    std::println("sent {} bytes to {}", sent.value(), server.to_string());

    std::array<std::byte, 2048> buf {};
    auto received { socket.recv_from(std::span<std::byte>(buf)) };

    if (!received.has_value())
    {

        std::println("recv_from failed");
        return 1;

    }

    const auto [bytes, peer] = received.value();
    const std::string reply { reinterpret_cast<const char*>(buf.data()), bytes };
    std::println("received {} bytes from {}: \"{}\"", bytes, peer.to_string(), reply);

    return 0;

}
