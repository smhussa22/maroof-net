#include <maroof/net/Server.hh>

#include <system_error>

namespace maroof::net
{

    Server::Server(const Endpoint& bind_addr)
    {
        if (auto r = m_socket.bind(bind_addr); !r.has_value())
        {
            throw std::system_error(std::make_error_code(std::errc::address_in_use),
                                    "Server: bind failed");
        }
        if (auto r = m_socket.set_nonblocking(); !r.has_value())
        {
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    "Server: set_nonblocking failed");
        }
    }

    std::size_t Server::tick()
    {
        std::size_t processed = 0;

        // Drain the socket of every datagram the kernel has queued.
        for (;;)
        {
            auto received = m_socket.recv_from(std::span<std::byte>(m_recv_buf));
            if (!received.has_value())
            {
                break;  // WouldBlock or genuine error — either way, nothing more to drain
            }
            const auto [bytes, peer] = received.value();

            auto parsed = parse(std::span<const std::byte>(m_recv_buf.data(), bytes));
            if (!parsed.has_value())
            {
                // Count malformed drops at the server level — connection-less events.
                continue;
            }
            handle_packet(peer, parsed->first, parsed->second);
            ++processed;
        }

        drive_tick();
        return processed;
    }

    void Server::run()
    {
        m_running = true;
        while (m_running) tick();
    }

    void Server::handle_packet(const Endpoint& peer, const PacketHeader& header,
                               std::span<const std::byte> payload)
    {

        auto it = m_connections.find(peer);

        // Hello: idempotent create → HelloAck. Fires on_connect only on the first establishment.
        if (header.type == PacketType::Hello)
        {
            const bool existed = (it != m_connections.end());
            if (!existed)
            {
                it = m_connections.emplace(peer, Connection(peer)).first;
            }
            auto ack = it->second.accept_hello(header.sequence);
            if (auto n = serialize(ack, {}, std::span<std::byte>(m_send_buf)); n.has_value())
            {
                (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), peer);
            }
            if (!existed && m_on_connect) m_on_connect(it->second);
            return;
        }

        // Unknown or disconnected peer: drop silently.
        if (it == m_connections.end() || it->second.state() != Connection::State::Established)
        {
            return;
        }

        auto& conn = it->second;
        conn.note_received(header.sequence);
        conn.note_activity(std::chrono::steady_clock::now());

        // Any inbound packet may carry a piggybacked ACK for one of our outstanding reliable sends.
        if (header.piggyback_ack_seq != NO_PIGGYBACK_ACK)
        {
            conn.handle_ack(header.piggyback_ack_seq);
        }

        if (header.type == PacketType::Ack)
        {
            conn.handle_ack(header.sequence);
            return;
        }

        if (header.type == PacketType::Ping)
        {
            auto pong = conn.on_ping(header);
            if (auto n = serialize(pong, {}, std::span<std::byte>(m_send_buf)); n.has_value())
            {
                (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), peer);
            }
            return;
        }

        if (header.type == PacketType::Pong)
        {
            conn.on_pong(header, std::chrono::steady_clock::now());
            return;
        }

        if (header.type != PacketType::Data) return;

        // Piggyback window: stash the ACK we'd normally send stand-alone. If the user's callback
        // triggers a send on this Connection, Connection::send_* pulls the pending via
        // take_pending_piggyback_ack() and bakes it into the outgoing header → free ACK.
        // If no send happens, we flush a stand-alone ACK after the callback returns.
        conn.set_pending_piggyback_ack(header.sequence);

        const bool should_ack = conn.receive_data(header, payload,
            [&](std::span<const std::byte> p)
            {
                if (m_on_receive) m_on_receive(conn, p, header.channel_id);
            });

        if (!should_ack)
        {
            // Channel refused the packet (beyond window / oversized) — don't ACK, don't piggyback.
            conn.clear_pending_piggyback_ack();
            return;
        }

        if (conn.pending_piggyback_ack().has_value())
        {
            // Callback didn't produce an outgoing packet → fall back to a stand-alone Ack.
            conn.clear_pending_piggyback_ack();
            auto ack = conn.make_ack(header.sequence);
            if (auto n = serialize(ack, {}, std::span<std::byte>(m_send_buf)); n.has_value())
            {
                (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), peer);
            }
        }

    }

    void Server::drive_tick()
    {
        const auto now = std::chrono::steady_clock::now();

        // Walk every connection, flush retransmits, remove any that transitioned to Disconnected.
        for (auto it = m_connections.begin(); it != m_connections.end(); )
        {
            auto& conn = it->second;
            auto retransmits = conn.tick(now);
            for (auto bytes : retransmits)
            {
                (void) m_socket.send_to(bytes, conn.peer());
            }

            if (conn.state() == Connection::State::Disconnected)
            {
                if (m_on_disconnect) m_on_disconnect(it->first);
                it = m_connections.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace maroof::net
