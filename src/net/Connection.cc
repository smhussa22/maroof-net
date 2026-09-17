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

    PacketHeader Connection::accept_hello(std::uint64_t hello_sequence)
    {

        // Seed the reliable receive window from the Hello, not from whichever
        // Data packet happens to arrive first - only on the first Hello we
        // see, so a duplicate Hello after Established can't undo progress.
        if (!m_next_expected_reliable_seq.has_value())
        {

            m_next_expected_reliable_seq = hello_sequence + 1;

        }

        m_state = State::Established;
        PacketHeader header { m_next_send_seq++, PacketType::HelloAck, 0, 0 };
        return header;

    }

    void Connection::on_hello_ack()
    {

        //   if state == Disconnected -> ignore (stray packet)

        if(m_state == State::Connecting)
        {

            m_state = State::Established;
            return;

        }

        // duplicate hello ack arrived late
        if(m_state == State::Established)
        {

            return;
        
        }

        if(m_state == State::Disconnected)
        {

            return;

        }

    }

    PacketHeader Connection::make_data_header(std::uint8_t channel_id)
    {

        PacketHeader header { m_next_send_seq++, PacketType::Data, channel_id, 0 };
        return header;
    
    }

    void Connection::note_received(std::uint64_t sequence)
    {

        if (sequence > m_highest_received_seq)
        {

            m_highest_received_seq = sequence;

        }

    }

    std::span<const std::byte> Connection::make_reliable_data(std::span<const std::byte> payload, std::uint8_t channel_id)
    {

        // Grab the sequence BEFORE incrementing so it lines up with the header
        const auto seq { m_next_send_seq++ };
        PacketHeader header { seq, PacketType::Data, channel_id, 0 };

        std::vector<std::byte> buf(PACKET_HEADER_WIRE_SIZE + payload.size());
        serialize(header, payload, std::span<std::byte>(buf)).value();

        PendingPacket pending 
        {
        
            .serialized  { std::move(buf) },
            .retransmit  { std::chrono::steady_clock::now() + INITIAL_TIMEOUT },
            .retry_count { 0 },
        
        };

        auto [it, inserted] = m_sent_reliable.emplace(seq, std::move(pending));

        return std::span<const std::byte>(it->second.serialized);

    }

    std::vector<std::span<const std::byte>> Connection::tick(std::chrono::steady_clock::time_point now)
    {

        std::vector<std::span<const std::byte>> retransmits {};

        for (auto& [seq, packet] : m_sent_reliable)
        {

            // timer hasn't fired yet for this one
            if (packet.retransmit > now)
            {

                continue;
            
            }

            // peer is unreachable
            if (packet.retry_count >= MAX_RETRIES)
            {

                m_state = State::Disconnected;
                return retransmits;

            }

            packet.retry_count++;

            // compute exponential backoff for the next timer.
            const std::uint32_t shift = packet.retry_count > 4u ? 4u : packet.retry_count;
            packet.retransmit = now + INITIAL_TIMEOUT * (1u << shift);
            retransmits.push_back(std::span<const std::byte>(packet.serialized));

        }

        return retransmits;

    }

    void Connection::handle_ack(std::uint64_t sequence)
    {

        m_sent_reliable.erase(sequence);

    }

    PacketHeader Connection::make_ack(std::uint64_t received_sequence)
    {

        return { received_sequence, PacketType::Ack, 0, 0 };

    }

} // namespace maroof::net
