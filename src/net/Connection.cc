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
        PacketHeader header { m_next_send_seq++, PacketType::HelloAck, 0, 0 };
        return header;

    }

    void Connection::on_hello_ack()
    {

        if (m_state == State::Connecting)
        {
            m_state = State::Established;
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

        auto retransmits = m_channel_0.tick(now);
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
        return m_channel_0.send(payload);
    }

    std::span<const std::byte> Connection::send_unreliable(std::span<const std::byte> payload)
    {
        return m_channel_1.send(payload);
    }

    std::span<const std::byte> Connection::send_sequenced(std::span<const std::byte> payload)
    {
        return m_channel_2.send(payload);
    }

} // namespace maroof::net
