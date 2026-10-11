#include <maroof/net/CpuAffinity.hh>
#include <maroof/net/LatencyHistogram.hh>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <string_view>
#include <unordered_map>

// TCP counterpart to latency_bench. Same CO-safe scheduling: pre-compute intended send times,
// busy-wait to each, record intended→echo-received. Non-blocking socket + 8-byte framing.

using namespace std::chrono_literals;
using namespace maroof::net;

int main(int argc, char* argv[])
{
    std::string_view server_ip { "127.0.0.1" };
    std::uint16_t    port      { 9500 };
    std::uint32_t    rate      { 10'000 };
    std::uint32_t    duration  { 5 };
    int              cpu       { -1 };

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg { argv[i] };
        auto next_val = [&](auto& dest) {
            if (i + 1 < argc) {
                std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), dest);
                ++i;
            }
        };
        if      (arg == "--server"   && i + 1 < argc) { server_ip = argv[++i]; }
        else if (arg == "--port"    ) next_val(port);
        else if (arg == "--rate"    ) next_val(rate);
        else if (arg == "--duration") next_val(duration);
        else if (arg == "--cpu"     ) next_val(cpu);
    }

    if (cpu >= 0 && !pin_current_thread_to(cpu))
    {
        std::println("[cpu] WARNING: failed to pin");
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, std::string(server_ip).c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::println("connect failed");
        return 1;
    }

    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    // Non-blocking so recv doesn't stall when the socket buffer is empty.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::println("[tcp] connected to {}:{}", server_ip, port);

    const auto t0       = std::chrono::steady_clock::now() + 100ms;
    const auto interval = std::chrono::nanoseconds { 1'000'000'000 / rate };
    const std::uint64_t total = static_cast<std::uint64_t>(rate) * duration;

    LatencyHistogram latency { 1ns, 60s };
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> intended;
    intended.reserve(total);

    std::uint64_t sent = 0;
    std::uint64_t acked = 0;

    // Staging buffer: TCP can give us partial frames; accumulate until we have a full 8 bytes.
    std::array<std::byte, 4096> stage {};
    std::size_t                 stage_len = 0;

    const auto deadline = t0 + std::chrono::seconds { duration } + 10s;

    while (acked < total && std::chrono::steady_clock::now() < deadline)
    {
        const auto now = std::chrono::steady_clock::now();

        // Pace sends against the schedule.
        while (sent < total)
        {
            const auto scheduled = t0 + sent * interval;
            if (scheduled > now) break;

            std::uint64_t idx = sent;
            const auto rc = ::send(fd, &idx, sizeof(idx), 0);
            if (rc < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                std::println("send error errno={}", errno);
                return 1;
            }
            intended.emplace(idx, scheduled);
            ++sent;
        }

        // Drain whatever echoes have come back.
        for (;;)
        {
            auto want = stage.size() - stage_len;
            auto rc   = ::recv(fd, stage.data() + stage_len, want, 0);
            if (rc < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                std::println("recv error errno={}", errno);
                return 1;
            }
            if (rc == 0) { std::println("peer closed"); return 1; }
            stage_len += static_cast<std::size_t>(rc);

            // Pull as many complete 8-byte frames as we have.
            std::size_t off = 0;
            while (stage_len - off >= sizeof(std::uint64_t))
            {
                std::uint64_t idx;
                std::memcpy(&idx, stage.data() + off, sizeof(idx));
                off += sizeof(idx);

                auto it = intended.find(idx);
                if (it != intended.end())
                {
                    auto sample = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - it->second);
                    latency.record(sample);
                    intended.erase(it);
                    ++acked;
                }
            }
            if (off > 0)
            {
                std::memmove(stage.data(), stage.data() + off, stage_len - off);
                stage_len -= off;
            }
        }
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::println("");
    std::println("=== TCP Latency Bench ===");
    std::println("target rate     : {} msgs/sec", rate);
    std::println("duration        : {} s", duration);
    std::println("messages sent   : {}", sent);
    std::println("acked           : {} / {}", acked, total);
    std::println("elapsed         : {} ms", elapsed_ms);
    std::println("");
    std::println("intended->echo latency (CO-safe):");
    std::println("  p50     : {} ns", latency.percentile( 50.0).count());
    std::println("  p99     : {} ns", latency.percentile( 99.0).count());
    std::println("  p99.9   : {} ns", latency.percentile( 99.9).count());
    std::println("  p99.99  : {} ns", latency.percentile(99.99).count());

    ::close(fd);
    return (acked == total) ? 0 : 1;
}
