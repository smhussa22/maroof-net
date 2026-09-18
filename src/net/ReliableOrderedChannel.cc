#include <maroof/net/ReliableOrderedChannel.hh>

namespace maroof::net
{

    std::span<const std::byte> ReliableOrderedChannel::send(std::span<const std::byte> payload)
    {

        const auto seq { m_next_send_seq++ };
        PacketHeader header { seq, PacketType::Data, CHANNEL_ID, 0 };

        std::vector<std::byte> buf(PACKET_HEADER_WIRE_SIZE + payload.size());
        serialize(header, payload, std::span<std::byte>(buf)).value();

        PendingPacket pending
        {
            .serialized  { std::move(buf) },
            .retransmit  { std::chrono::steady_clock::now() + INITIAL_TIMEOUT },
            .retry_count { 0 },
        };

        auto [it, inserted] = m_pending.emplace(seq, std::move(pending));
        return std::span<const std::byte>(it->second.serialized);

    }

    std::vector<std::span<const std::byte>> ReliableOrderedChannel::tick(std::chrono::steady_clock::time_point now)
    {

        std::vector<std::span<const std::byte>> retransmits {};

        for (auto& [seq, packet] : m_pending)
        {

            if (packet.retransmit > now)
            {
                continue;
            }

            if (packet.retry_count >= MAX_RETRIES)
            {
                m_dead = true;
                return retransmits;
            }

            packet.retry_count++;

            const std::uint32_t shift = packet.retry_count > 4u ? 4u : packet.retry_count;
            packet.retransmit = now + INITIAL_TIMEOUT * (1u << shift);
            retransmits.push_back(std::span<const std::byte>(packet.serialized));

        }

        return retransmits;

    }

    void ReliableOrderedChannel::handle_ack(std::uint64_t sequence)
    {
        m_pending.erase(sequence);
    }

    PacketHeader ReliableOrderedChannel::make_ack(std::uint64_t received_sequence)
    {
        return { received_sequence, PacketType::Ack, CHANNEL_ID, 0 };
    }

} // namespace maroof::net
