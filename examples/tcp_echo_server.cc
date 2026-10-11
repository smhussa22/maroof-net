#include <maroof/net/CpuAffinity.hh>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <string_view>

// Minimal single-client TCP echo server for the latency-benchmark baseline. Blocking recv
// into a buffer, blocking send back. TCP_NODELAY so each 8-byte message goes out immediately.

using namespace maroof::net;

int main(int argc, char* argv[])
{
    std::uint16_t port { 9500 };
    int           cpu  { -1 };

    if (argc > 1)
    {
        const std::string_view arg { argv[1] };
        std::from_chars(arg.data(), arg.data() + arg.size(), port);
    }
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view arg { argv[i] };
        if (arg == "--cpu" && i + 1 < argc)
        {
            std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), cpu);
            ++i;
        }
    }
    if (cpu >= 0 && !pin_current_thread_to(cpu))
    {
        std::println("[cpu] WARNING: failed to pin");
    }

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::println("bind failed");
        return 1;
    }
    ::listen(listen_fd, 1);
    std::println("tcp_echo_server listening on 0.0.0.0:{} (cpu={})", port, cpu);

    int client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) { std::println("accept failed"); return 1; }

    // Nagle off: 8-byte messages should hit the wire immediately, not sit waiting for peer buffer.
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    // Simple framed echo: pull exactly 8 bytes, echo back exactly 8 bytes, repeat.
    std::array<std::byte, 8> buf {};
    for (;;)
    {
        std::size_t have = 0;
        while (have < buf.size())
        {
            ssize_t n = ::recv(client_fd, buf.data() + have, buf.size() - have, 0);
            if (n <= 0) { ::close(client_fd); ::close(listen_fd); return 0; }
            have += static_cast<std::size_t>(n);
        }
        std::size_t sent = 0;
        while (sent < buf.size())
        {
            ssize_t n = ::send(client_fd, buf.data() + sent, buf.size() - sent, 0);
            if (n <= 0) { ::close(client_fd); ::close(listen_fd); return 0; }
            sent += static_cast<std::size_t>(n);
        }
    }
}
