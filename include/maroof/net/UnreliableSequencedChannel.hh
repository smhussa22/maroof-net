#ifndef MAROOF_NET_UNRELIABLE_SEQUENCED_CHANNEL_HH
#define MAROOF_NET_UNRELIABLE_SEQUENCED_CHANNEL_HH

#include <maroof/net/PacketHeader.hh>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace maroof::net
{

class UnreliableSequencedChannel
{
public:

    static constexpr std::uint8_t CHANNEL_ID { 2 };

    // Serialize payload and return wire bytes. Send before the next call to send().
    std::span<const std::byte> send(std::span<const std::byte> payload);

    // Latest-wins delivery. Deliver only if sequence is strictly newer than m_highest_received_seq.
    // Drop silently if stale — an old update arriving late is worse than missing.
    template <typename OnProcessPayload>
    void receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver);

private:

    std::uint64_t m_next_send_seq { 0 };
    std::vector<std::byte> m_send_buf {};

    // Unset until first packet arrives; lazy init avoids assuming the stream starts at seq 0.
    std::optional<std::uint64_t> m_highest_received_seq {};

};


// Latest-wins delivery. Deliver only if sequence is strictly newer than m_highest_received_seq.
    // Drop silently if stale — an old update arriving late is worse than missing.
template <typename OnProcessPayload>
void UnreliableSequencedChannel::receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver)
{

    // if this packet is not newer than the last one we delviered
    if (m_highest_received_seq.has_value() && sequence <= *m_highest_received_seq)
    {

        return;

    }

    m_highest_received_seq = sequence;
    deliver(payload);

}

} // namespace maroof::net

#endif // MAROOF_NET_UNRELIABLE_SEQUENCED_CHANNEL_HH
