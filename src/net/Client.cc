#include <maroof/net/Client.hh>

#include <system_error>

namespace maroof::net
{

    Client::Client(const Endpoint& server)
        : m_conn { server }
    {
        if (auto r = m_socket.set_nonblocking(); !r.has_value())
        {
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    "Client: set_nonblocking failed");
        }
    }

    bool Client::connect()
    {
        if (m_conn.state() != Connection::State::Disconnected) return false;

        auto hello = m_conn.make_hello();
        auto n = serialize(hello, {}, std::span<std::byte>(m_send_buf));
        if (!n.has_value()) return false;
        (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), m_conn.peer());

        m_last_hello_at  = std::chrono::steady_clock::now();
        m_hello_attempts = 1;
        return true;
    }

    void Client::send_reliable(std::span<const std::byte> payload)
    {
        for (auto bytes : m_conn.send_reliable(payload))
        {
            (void) m_socket.send_to(bytes, m_conn.peer());
        }
    }

    void Client::send_unreliable(std::span<const std::byte> payload)
    {
        auto bytes = m_conn.send_unreliable(payload);
        (void) m_socket.send_to(bytes, m_conn.peer());
    }

    void Client::send_sequenced(std::span<const std::byte> payload)
    {
        auto bytes = m_conn.send_sequenced(payload);
        (void) m_socket.send_to(bytes, m_conn.peer());
    }

    void Client::send_ping()
    {
        auto ping = m_conn.make_ping(std::chrono::steady_clock::now());
        if (auto n = serialize(ping, {}, std::span<std::byte>(m_send_buf)); n.has_value())
        {
            (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), m_conn.peer());
        }
    }

    std::size_t Client::tick()
    {
        std::size_t processed = 0;

        for (;;)
        {
            auto received = m_socket.recv_from(std::span<std::byte>(m_recv_buf));
            if (!received.has_value()) break;

            const auto [bytes, peer] = received.value();
            if (!(peer == m_conn.peer())) continue;  // packet from someone else

            auto parsed = parse(std::span<const std::byte>(m_recv_buf.data(), bytes));
            if (!parsed.has_value()) continue;
            handle_packet(parsed->first, parsed->second);
            ++processed;
        }

        const auto now = std::chrono::steady_clock::now();
        retry_hello_if_needed(now);

        // Fire retransmits and watch for the heartbeat/retry-exhaustion death signal.
        const auto prior_state = m_conn.state();
        auto retransmits = m_conn.tick(now);
        for (auto bytes : retransmits)
        {
            (void) m_socket.send_to(bytes, m_conn.peer());
        }
        if (prior_state == Connection::State::Established
            && m_conn.state() == Connection::State::Disconnected
            && m_on_disconnect)
        {
            m_on_disconnect();
        }

        return processed;
    }

    void Client::run()
    {
        m_running = true;
        while (m_running) tick();
    }

    void Client::handle_packet(const PacketHeader& header, std::span<const std::byte> payload)
    {

        m_conn.note_received(header.sequence);
        m_conn.note_activity(std::chrono::steady_clock::now());

        // Any inbound packet may carry a piggybacked ACK for one of our reliable sends.
        if (header.piggyback_ack_seq != NO_PIGGYBACK_ACK)
        {
            m_conn.handle_ack(header.piggyback_ack_seq);
        }

        if (header.type == PacketType::HelloAck)
        {
            m_conn.on_hello_ack();
            if (!m_fired_connect && m_conn.state() == Connection::State::Established)
            {
                m_fired_connect = true;
                if (m_on_connect) m_on_connect();
            }
            return;
        }

        if (header.type == PacketType::Ack)
        {
            m_conn.handle_ack(header.sequence);
            return;
        }

        if (header.type == PacketType::Ping)
        {
            auto pong = m_conn.on_ping(header);
            if (auto n = serialize(pong, {}, std::span<std::byte>(m_send_buf)); n.has_value())
            {
                (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), m_conn.peer());
            }
            return;
        }

        if (header.type == PacketType::Pong)
        {
            m_conn.on_pong(header, std::chrono::steady_clock::now());
            return;
        }

        if (header.type != PacketType::Data) return;

        // Set pending piggyback ACK; if the user's callback triggers an outgoing send, the ACK
        // rides along free. Otherwise we flush stand-alone after the callback.
        m_conn.set_pending_piggyback_ack(header.sequence);

        const bool should_ack = m_conn.receive_data(header, payload,
            [&](std::span<const std::byte> p)
            {
                if (m_on_receive) m_on_receive(p, header.channel_id);
            });

        if (!should_ack)
        {
            m_conn.clear_pending_piggyback_ack();
            return;
        }

        if (m_conn.pending_piggyback_ack().has_value())
        {
            m_conn.clear_pending_piggyback_ack();
            auto ack = m_conn.make_ack(header.sequence);
            if (auto n = serialize(ack, {}, std::span<std::byte>(m_send_buf)); n.has_value())
            {
                (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), m_conn.peer());
            }
        }

    }

    void Client::retry_hello_if_needed(std::chrono::steady_clock::time_point now)
    {
        if (m_conn.state() != Connection::State::Connecting) return;
        if (m_hello_attempts >= MAX_HELLO_ATTEMPTS) return;
        if (now - m_last_hello_at < HELLO_RETRY_INTERVAL) return;

        // Resend the SAME hello (make_hello increments the handshake counter — we want retry,
        // not a fresh handshake. The server treats duplicates as idempotent anyway.)
        auto hello = m_conn.make_hello();
        if (auto n = serialize(hello, {}, std::span<std::byte>(m_send_buf)); n.has_value())
        {
            (void) m_socket.send_to(std::span<const std::byte>(m_send_buf.data(), *n), m_conn.peer());
        }
        m_last_hello_at = now;
        ++m_hello_attempts;
    }

} // namespace maroof::net
