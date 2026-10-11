#include <maroof/net/Connection.hh>
#include <maroof/net/CpuAffinity.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/IoUringReactor.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UdpSocket.hh>

#include <sys/socket.h>
#include <netinet/in.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <string_view>
#include <unordered_map>

// Parallel to echo_server.cc but using io_uring instead of epoll + recvmmsg + sendmmsg.
// Pre-posts N recvmsg ops, each with its own buffer. On each completion: process the datagram,
// submit a sendmsg reply, repost the recv slot. Submissions happen without touching the kernel;
// one io_uring_enter syscall per batch (less with SQPOLL, which this demo doesn't enable).

using namespace maroof::net;

namespace
{
    // Each outstanding recv op owns its buffer, sockaddr, iovec, and msghdr. The msghdr points
    // into the other fields; keep this whole struct alive from submission until completion.
    struct RecvCtx
    {
        std::array<std::byte, 2048> buf;
        sockaddr_in                 peer_sa;
        iovec                       iov;
        msghdr                      hdr;
    };

    // Same shape for sends. The byte buffer is filled by the main loop before submission.
    struct SendCtx
    {
        std::array<std::byte, 2048> buf;
        std::size_t                 len { 0 };
        sockaddr_in                 peer_sa;
        iovec                       iov;
        msghdr                      hdr;
        bool                        in_use { false };
    };

    void arm_recv(RecvCtx& ctx)
    {
        ctx.iov.iov_base      = ctx.buf.data();
        ctx.iov.iov_len       = ctx.buf.size();
        ctx.hdr.msg_name      = &ctx.peer_sa;
        ctx.hdr.msg_namelen   = sizeof(sockaddr_in);
        ctx.hdr.msg_iov       = &ctx.iov;
        ctx.hdr.msg_iovlen    = 1;
        ctx.hdr.msg_control   = nullptr;
        ctx.hdr.msg_controllen = 0;
        ctx.hdr.msg_flags     = 0;
    }

    void arm_send(SendCtx& ctx, std::span<const std::byte> payload, const Endpoint& peer)
    {
        std::memcpy(ctx.buf.data(), payload.data(), payload.size());
        ctx.len               = payload.size();
        ctx.peer_sa           = peer.to_sockaddr_in();
        ctx.iov.iov_base      = ctx.buf.data();
        ctx.iov.iov_len       = ctx.len;
        ctx.hdr.msg_name      = &ctx.peer_sa;
        ctx.hdr.msg_namelen   = sizeof(sockaddr_in);
        ctx.hdr.msg_iov       = &ctx.iov;
        ctx.hdr.msg_iovlen    = 1;
        ctx.hdr.msg_control   = nullptr;
        ctx.hdr.msg_controllen = 0;
        ctx.hdr.msg_flags     = 0;
        ctx.in_use            = true;
    }
}

int main(int argc, char* argv[])
{

    std::uint16_t port { 9000 };
    int           cpu  { -1 };
    bool          quiet { false };

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
        else if (arg == "--quiet")
        {
            quiet = true;
        }
    }

    if (cpu >= 0)
    {
        if (pin_current_thread_to(cpu)) std::println("[cpu] pinned to core {}", cpu);
        else                            std::println("[cpu] WARNING: failed to pin to core {}", cpu);
    }

    UdpSocket socket;
    if (auto r { socket.bind(Endpoint("0.0.0.0", port)) }; !r.has_value())
    {
        std::println("bind failed");
        return 1;
    }

    std::println("echo server listening on 0.0.0.0:{} (io_uring)", port);

    IoUringReactor reactor { 256 };

    // Pre-posted recv slots. 32 outstanding is more than enough for a demo workload.
    static constexpr std::size_t N_RECV = 32;
    static constexpr std::size_t N_SEND = 64;

    std::array<RecvCtx, N_RECV> recvs {};
    std::array<SendCtx, N_SEND> sends {};

    // Prime the pump: submit every recv slot upfront.
    for (auto& r : recvs)
    {
        arm_recv(r);
        if (!reactor.submit_recvmsg(socket.fd(), &r.hdr, &r))
        {
            std::println("submit_recvmsg failed at startup");
            return 1;
        }
    }
    if (!reactor.submit().has_value())
    {
        std::println("initial submit failed");
        return 1;
    }

    // Reap-up-to-this-many completions per wait, keeps worst-case loop latency bounded.
    std::array<IoUringReactor::Completion, 32> comps {};
    std::unordered_map<Endpoint, Connection> connections;

    for (;;)
    {

        auto n_c = reactor.wait_batch(std::span<IoUringReactor::Completion>(comps));
        if (!n_c.has_value())
        {
            std::println("wait_batch failed");
            return 1;
        }

        for (std::size_t i = 0; i < *n_c; ++i)
        {

            void* data = comps[i].data;
            int   res  = comps[i].result;

            // Identify by pointer: recvs vs sends live in separate contiguous arrays.
            if (data >= &recvs.front() && data <= &recvs.back())
            {

                auto& ctx = *static_cast<RecvCtx*>(data);
                if (res < 0)
                {
                    // Something went wrong with this slot — repost it blind.
                    arm_recv(ctx);
                    (void) reactor.submit_recvmsg(socket.fd(), &ctx.hdr, &ctx);
                    continue;
                }

                const std::size_t len = static_cast<std::size_t>(res);
                Endpoint peer = Endpoint::from_sockaddr_in(ctx.peer_sa);

                auto parsed = parse(std::span<const std::byte>(ctx.buf.data(), len));
                if (parsed.has_value())
                {

                    const auto& [header, payload] = parsed.value();
                    auto it = connections.find(peer);

                    if (header.type == PacketType::Hello)
                    {
                        if (it == connections.end())
                            it = connections.emplace(peer, Connection(peer)).first;

                        auto ack = it->second.accept_hello(header.sequence);

                        // Serialize into a free send slot.
                        for (auto& s : sends)
                        {
                            if (s.in_use) continue;
                            std::array<std::byte, 2048> tmp;
                            auto n = serialize(ack, {}, std::span<std::byte>(tmp));
                            if (n.has_value())
                            {
                                arm_send(s, std::span<const std::byte>(tmp.data(), *n), peer);
                                (void) reactor.submit_sendmsg(socket.fd(), &s.hdr, &s);
                                if (!quiet) std::println("[handshake] accepted from {}", peer.to_string());
                            }
                            break;
                        }
                    }
                    else if (it != connections.end() && it->second.state() == Connection::State::Established)
                    {
                        it->second.note_received(header.sequence);
                        it->second.note_activity(std::chrono::steady_clock::now());

                        if (header.type == PacketType::Data)
                        {
                            bool should_ack = it->second.receive_data(header, payload,
                                [&](std::span<const std::byte> p)
                                {
                                    // Echo as unreliable — grab a send slot.
                                    auto reply_bytes = it->second.send_unreliable(p);
                                    for (auto& s : sends)
                                    {
                                        if (s.in_use) continue;
                                        arm_send(s, reply_bytes, peer);
                                        (void) reactor.submit_sendmsg(socket.fd(), &s.hdr, &s);
                                        break;
                                    }
                                });

                            if (should_ack)
                            {
                                auto ack = it->second.make_ack(header.sequence);
                                for (auto& s : sends)
                                {
                                    if (s.in_use) continue;
                                    std::array<std::byte, 2048> tmp;
                                    auto n = serialize(ack, {}, std::span<std::byte>(tmp));
                                    if (n.has_value())
                                    {
                                        arm_send(s, std::span<const std::byte>(tmp.data(), *n), peer);
                                        (void) reactor.submit_sendmsg(socket.fd(), &s.hdr, &s);
                                    }
                                    break;
                                }
                            }
                        }
                    }

                }

                // Repost the recv slot so the ring stays saturated.
                arm_recv(ctx);
                (void) reactor.submit_recvmsg(socket.fd(), &ctx.hdr, &ctx);

            }
            else
            {
                // Send completion — just free the slot. We ignore partial sends on UDP (datagrams
                // are atomic; the kernel either delivers the whole thing or fails).
                auto& ctx = *static_cast<SendCtx*>(data);
                ctx.in_use = false;
            }

        }

        // One io_uring_enter syscall flushes everything we queued during this batch.
        (void) reactor.submit();

    }

    return 0;

}
