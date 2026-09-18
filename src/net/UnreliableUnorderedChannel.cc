#include <maroof/net/UnreliableUnorderedChannel.hh>

namespace maroof::net
{

    // Serialize payload into the internal buffer and return a span of the wire bytes.
    // No pending registration — fire and forget. Send the span before the next call to send().
    std::span<const std::byte> UnreliableUnorderedChannel::send(std::span<const std::byte> payload)
    {
        // serialize into it, return span

        auto seq { m_next_send_seq++ };
        PacketHeader header { seq, PacketType::Data, CHANNEL_ID, 0 };
        
        m_send_buf.resize(PACKET_HEADER_WIRE_SIZE + payload.size());
        serialize(header, payload, std::span<std::byte>(m_send_buf)).value();

        return std::span<const std::byte>(m_send_buf);
        
    }

} // namespace maroof::net
