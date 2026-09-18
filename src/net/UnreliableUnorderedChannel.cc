#include <maroof/net/UnreliableUnorderedChannel.hh>

namespace maroof::net
{

    std::span<const std::byte> UnreliableUnorderedChannel::send(std::span<const std::byte> payload)
    {
        // TODO: grab seq (m_next_send_seq++), build header, resize m_send_buf, serialize into it, return span
    }

} // namespace maroof::net
