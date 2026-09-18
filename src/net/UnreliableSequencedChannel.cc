#include <maroof/net/UnreliableSequencedChannel.hh>

namespace maroof::net
{

    std::span<const std::byte> UnreliableSequencedChannel::send(std::span<const std::byte> payload)
    {
        // TODO: same as UnreliableUnorderedChannel::send — grab seq, build header, serialize, return span
    }

} // namespace maroof::net
