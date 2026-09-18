#include <maroof/net/UnreliableSequencedChannel.hh>

namespace maroof::net
{

    std::span<const std::byte> UnreliableSequencedChannel::send(std::span<const std::byte> payload)
    {

        auto seq { m_next_send_seq++ };
        PacketHeader header { seq, PacketType::Data, CHANNEL_ID, 0 };
        m_send_buf.resize(PACKET_HEADER_WIRE_SIZE + payload.size());
        serialize(header, payload, std::span<std::byte>{ m_send_buf }).value();
        return std::span<const std::byte>(m_send_buf);

    }

} // namespace maroof::net
