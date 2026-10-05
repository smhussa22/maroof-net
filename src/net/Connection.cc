#include <maroof/net/Connection.hh>

namespace maroof::net
{

    Connection::Connection(const Endpoint& peer) : m_peer { peer } { }

    PacketHeader Connection::make_hello()
    {

        m_state = State::Connecting;
        PacketHeader header { m_next_send_seq++, PacketType::Hello, 0, 0 };
        return header;

    }

    PacketHeader Connection::accept_hello(std::uint64_t /*hello_sequence*/)
    {

        // Seed the reliable channel's receive window to 0 — after the channel refactor, the
        // channel's send counter is independent of the handshake counter and always starts at 0.
        // seed_receive is idempotent, so a duplicate Hello won't rewind delivery progress.
        m_channel_0.seed_receive(0);
        m_state = State::Established;
        // Seed the heartbeat timer: the Hello itself is a fresh "just heard from peer" signal.
        // Without this, the first tick() would fire the death check against time_point{}'s epoch.
        m_last_heard_from = std::chrono::steady_clock::now();
        PacketHeader header { m_next_send_seq++, PacketType::HelloAck, 0, 0 };
        return header;

    }

    void Connection::on_hello_ack()
    {

        if (m_state == State::Connecting)
        {
            m_state = State::Established;
            m_last_heard_from = std::chrono::steady_clock::now();
        }

        // Established: duplicate late HelloAck, ignore.
        // Disconnected: stray packet, ignore.

    }

    void Connection::note_received(std::uint64_t sequence)
    {

        if (sequence > m_highest_received_seq)
        {
            m_highest_received_seq = sequence;
        }

    }

    std::vector<std::span<const std::byte>> Connection::tick(std::chrono::steady_clock::time_point now)
    {

        // Heartbeat death: no packet from peer within HEARTBEAT_DEATH means peer is unreachable.
        // Only meaningful once Established — during Connecting the handshake retry loop drives death.
        if (m_state == State::Established && now - m_last_heard_from > HEARTBEAT_DEATH)
        {
            m_state = State::Disconnected;
            return {};
        }

        auto retransmits = m_channel_0.tick(now, m_smoothed_rtt);
        if (m_channel_0.dead())
        {
            m_state = State::Disconnected;
        }
        return retransmits;

    }

    void Connection::handle_ack(std::uint64_t sequence)
    {
        m_channel_0.handle_ack(sequence);
    }

    PacketHeader Connection::make_ack(std::uint64_t received_sequence)
    {
        return m_channel_0.make_ack(received_sequence);
    }

    std::span<const std::byte> Connection::send_reliable(std::span<const std::byte> payload)
    {
        return m_channel_0.send(payload, m_smoothed_rtt);
    }

    std::span<const std::byte> Connection::send_unreliable(std::span<const std::byte> payload)
    {
        return m_channel_1.send(payload);
    }

    std::span<const std::byte> Connection::send_sequenced(std::span<const std::byte> payload)
    {
        return m_channel_2.send(payload);
    }


    PacketHeader Connection::make_ping(std::chrono::steady_clock::time_point now)
    {

        PacketHeader header { m_next_ping_seq, PacketType::Ping, 0, 0 };
        m_ping_send_times.emplace(m_next_ping_seq, now);
        ++m_next_ping_seq;
        return header;

    }

    PacketHeader Connection::on_ping(const PacketHeader& ping) const noexcept
    {

        // TODO: return a Pong header echoing ping.sequence. channel_id and flags = 0.
        PacketHeader header { ping.sequence, PacketType::Pong, 0, 0 };
        return header;

    }

    void Connection::on_pong(const PacketHeader& pong, std::chrono::steady_clock::time_point now)
    {

        auto it { m_ping_send_times.find(pong.sequence) };
        if (it == m_ping_send_times.end())
            return;
        
        auto send_time { it->second };
        auto sample { now - send_time };

        // smooth the rtt with an EWMA fold copied from tcp's rto estimator value
        if (m_smoothed_rtt.has_value())
            *m_smoothed_rtt = *m_smoothed_rtt * 7 / 8 + sample / 8; 
        else
            m_smoothed_rtt = duration_cast<std::chrono::nanoseconds>(sample);

        m_ping_send_times.erase(it);

    }

} // namespace maroof::net
