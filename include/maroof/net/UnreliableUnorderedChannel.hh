#ifndef MAROOF_NET_UNRELIABLE_UNORDERED_CHANNEL_HH
#define MAROOF_NET_UNRELIABLE_UNORDERED_CHANNEL_HH

#include <maroof/net/PacketHeader.hh>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace maroof::net
{

class UnreliableUnorderedChannel
{
public:

    static constexpr std::uint8_t CHANNEL_ID { 1 };

    // Serialize payload into the internal buffer and return a span of the wire bytes.
    // No pending registration — fire and forget. Send the span before the next call to send().
    // piggyback_ack: if present, the receiver will treat it as an ACK for its reliable channel.
    std::span<const std::byte> send(std::span<const std::byte> payload,
                                    std::optional<std::uint64_t> piggyback_ack = std::nullopt);

    // Deliver payload immediately. No dedup, no ordering, no state — every packet fires deliver.
    template <typename OnProcessPayload>
    void receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver);

private:

    std::uint64_t m_next_send_seq { 0 };
    std::vector<std::byte> m_send_buf {};  // reused each send; no persistent storage needed

};

template <typename OnProcessPayload>
void UnreliableUnorderedChannel::receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver)
{
    
    deliver(payload);

}

} // namespace maroof::net

#endif // MAROOF_NET_UNRELIABLE_UNORDERED_CHANNEL_HH
