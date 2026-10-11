#include <maroof/net/Connection.hh>
#include <maroof/net/CpuAffinity.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/EpollReactor.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>
#include <maroof/net/UdpSocket.hh>

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
#include <vector>

// Production-ish IO pattern: epoll_wait blocks until the socket is readable, then recvmmsg
// drains everything in one syscall and sendmmsg batches the replies. Single-fd server so epoll
// is mostly for the API — the real wins are from the batched recv/send.

using namespace maroof::net;

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
    // epoll needs non-blocking so the drain loop can keep going until EAGAIN without stalling.
    if (!socket.set_nonblocking().has_value())
    {
        std::println("set_nonblocking failed");
        return 1;
    }

    std::println("echo server listening on 0.0.0.0:{} (epoll + recvmmsg + sendmmsg)", port);

    EpollReactor reactor;
    if (auto r = reactor.add_read(socket.fd(), &socket); !r.has_value())
    {
        std::println("epoll add_read failed");
        return 1;
    }

    std::unordered_map<Endpoint, Connection> connections;

    // Receive side: fixed-size batch of slots backed by one slab of memory.
    static constexpr std::size_t BATCH = UdpSocket::BATCH_LIMIT;
    static constexpr std::size_t MTU   = 2048;

    std::array<std::array<std::byte, MTU>, BATCH> recv_slab {};
    std::array<UdpSocket::RecvSlot, BATCH>        recv_slots {};
    for (std::size_t i = 0; i < BATCH; ++i)
    {
        recv_slots[i].buf = std::span<std::byte>(recv_slab[i]);
    }

    // Send side: slot array + persistent byte vectors for serialized bytes we hand to sendmmsg.
    // Keep the vectors alive for the duration of the sendmmsg call via one owning array per slot.
    std::vector<std::array<std::byte, MTU>> send_slab(BATCH);
    std::array<UdpSocket::SendSlot, BATCH>  send_slots {};
    std::size_t pending_sends = 0;

    std::array<EpollReactor::Event, 4> ev_buf {};

    for (;;)
    {

        auto n_events = reactor.wait(std::span<EpollReactor::Event>(ev_buf), std::chrono::milliseconds { -1 });
        if (!n_events.has_value())
        {
            std::println("epoll_wait failed");
            return 1;
        }

        // Only one fd registered, so drain the socket on any wakeup. Loop until recvmmsg
        // returns < BATCH (which signals the kernel queue is empty).
        for (;;)
        {

            auto got = socket.recv_many(std::span<UdpSocket::RecvSlot>(recv_slots));
            if (!got.has_value())
            {
                if (got.error() == ErrorCode::WouldBlock) break;  // EAGAIN — socket drained
                std::println("recv_many error");
                return 1;
            }

            for (std::size_t i = 0; i < *got; ++i)
            {

                const auto& slot = recv_slots[i];
                auto parsed = parse(std::span<const std::byte>(slot.buf.data(), slot.received));
                if (!parsed.has_value())
                {
                    if (!quiet) std::println("dropped malformed packet from {}", slot.peer.to_string());
                    continue;
                }

                const auto& [header, payload] = parsed.value();
                auto it = connections.find(slot.peer);

                // Hello: idempotent create + HelloAck. The ack goes out via the normal
                // sendmmsg batch at the bottom of this iteration.
                if (header.type == PacketType::Hello)
                {
                    if (it == connections.end())
                    {
                        it = connections.emplace(slot.peer, Connection(slot.peer)).first;
                    }
                    auto ack = it->second.accept_hello(header.sequence);
                    if (pending_sends < BATCH)
                    {
                        auto n = serialize(ack, {}, std::span<std::byte>(send_slab[pending_sends]));
                        if (n.has_value())
                        {
                            send_slots[pending_sends].bytes = std::span<const std::byte>(
                                send_slab[pending_sends].data(), *n);
                            send_slots[pending_sends].peer  = slot.peer;
                            ++pending_sends;
                            if (!quiet) std::println("[handshake] accepted from {}", slot.peer.to_string());
                        }
                    }
                    continue;
                }

                if (it == connections.end() || it->second.state() != Connection::State::Established)
                {
                    continue;
                }

                it->second.note_received(header.sequence);
                it->second.note_activity(std::chrono::steady_clock::now());

                if (header.type == PacketType::Data)
                {

                    bool should_ack = it->second.receive_data(header, payload,
                        [&](std::span<const std::byte> p)
                        {
                            // Echo as unreliable Data — send immediately on its own, not batched,
                            // because send_unreliable returns one span and the demo doesn't try
                            // to pool echo-reply bytes with the Ack reply bytes.
                            auto reply_bytes = it->second.send_unreliable(p);
                            auto one_slot = UdpSocket::SendSlot { reply_bytes, slot.peer };
                            auto _ = socket.send_many(std::span<const UdpSocket::SendSlot>(&one_slot, 1));
                            if (!quiet)
                                std::println("[echo] {} bytes seq {} from {}",
                                             p.size(), header.sequence, slot.peer.to_string());
                        });

                    if (should_ack && pending_sends < BATCH)
                    {
                        auto ack = it->second.make_ack(header.sequence);
                        auto n = serialize(ack, {}, std::span<std::byte>(send_slab[pending_sends]));
                        if (n.has_value())
                        {
                            send_slots[pending_sends].bytes = std::span<const std::byte>(
                                send_slab[pending_sends].data(), *n);
                            send_slots[pending_sends].peer  = slot.peer;
                            ++pending_sends;
                        }
                    }

                }

            }

            // If recvmmsg filled fewer than BATCH, the socket is drained — exit the inner loop
            // and go flush the sends we've queued.
            if (*got < BATCH) break;

        }

        // Batched send of all the ACKs/HelloAcks accumulated during this drain pass. Loops if
        // sendmmsg processes fewer than requested (rare on healthy sockets).
        std::size_t sent_so_far = 0;
        while (sent_so_far < pending_sends)
        {
            auto chunk = std::span<const UdpSocket::SendSlot>(
                send_slots.data() + sent_so_far, pending_sends - sent_so_far);
            auto r = socket.send_many(chunk);
            if (!r.has_value()) break;
            if (*r == 0) break;
            sent_so_far += *r;
        }
        pending_sends = 0;

    }

    return 0;

}
